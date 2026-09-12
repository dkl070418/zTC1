/*
 * ZControl 局域网 UDP 发现（阶段 A：只应答 device report，不处理控制命令）
 *
 * PC: bind 10181 → 广播到 255.255.255.255:10182 {"cmd":"device report"}
 * 设备: bind 10182 → 单播回 来源IP:10181
 *   {"name":"...","mac":"<12hex小写>","type":1,"type_name":"zTC1","ip":"..."}
 *
 * SoftAP 与 STA 都可能用：IP 从 ip_status 取（AP 默认 192.168.0.1）。
 * WiFi 切换后 socket 可能失效，线程里失败就关掉重 bind。
 */
#include "main.h"
#include "user_wifi.h"
#include "user_udp_discover.h"
#include "zcontrol/user_zcontrol.h"
#include "http_server/web_log.h"

#include "mico_rtos.h"
#include "mico_socket.h"
#include "SocketUtils.h"

#include <string.h>
#include <stdio.h>

/* RefreshStatus 查询 JSON 约 400B，控制包更短 */
#define UDP_RECV_MAX 768
#define UDP_SEND_MAX 1024

static volatile bool udp_disc_should_exit = false;
static volatile bool udp_disc_running = false;

static int UdpDiscOpenSocket(void)
{
    int fd;
    struct sockaddr_in addr;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_DISCOVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void UdpDiscCloseSocket(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* 包可能无 NUL 结尾，先拷到栈上再 strstr */
static bool UdpDiscIsReport(const char *buf, int len)
{
    char tmp[UDP_RECV_MAX];

    if (buf == NULL || len <= 0) return false;
    if (len >= (int) sizeof(tmp)) len = (int) sizeof(tmp) - 1;
    memcpy(tmp, buf, (size_t) len);
    tmp[len] = 0;
    return strstr(tmp, "device report") != NULL;
}

static void UdpDiscSendReport(int fd, const struct sockaddr_in *from)
{
    char name[48];
    char mac_lc[13];
    char send_buf[UDP_SEND_MAX];
    char ip_str[16];
    struct sockaddr_in to;
    int n, i, len;

    if (fd < 0 || from == NULL) return;

    /* ZControl 样例/云同步都按 12 位小写 hex；str_mac 是大写 */
    for (i = 0; i < 12; i++) {
        char c = str_mac[i];
        mac_lc[i] = (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
    }
    mac_lc[12] = 0;

    strncpy(name, sys_config->micoSystemConfig.name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    /* 名字里若混进引号会截断 JSON，简单剔掉 */
    for (i = 0; name[i]; i++) {
        if (name[i] == '"' || name[i] == '\\') name[i] = '-';
    }

    strncpy(ip_str, ip_status.ip, sizeof(ip_str) - 1);
    ip_str[sizeof(ip_str) - 1] = 0;

    n = snprintf(send_buf, sizeof(send_buf),
                 "{\"name\":\"%s\",\"mac\":\"%s\",\"type\":1,\"type_name\":\"zTC1\",\"ip\":\"%s\"}",
                 name, mac_lc, ip_str);
    if (n <= 0 || n >= (int) sizeof(send_buf)) {
        return;
    }

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    /* ZControl 固定监听本机 10181；不依赖来源端口 */
    to.sin_port = htons(UDP_DISCOVER_PEER_PORT);
    to.sin_addr = from->sin_addr;

    len = (int) sendto(fd, send_buf, (size_t) n, 0, (struct sockaddr *) &to, sizeof(to));
    if (len != n) {
        udp_log("UDP discover send fail ret=%d", len);
    } else {
        udp_log("UDP discover reply to %s:%d len=%d", ip_str, UDP_DISCOVER_PEER_PORT, n);
    }
}

static void UdpDiscoverThread(mico_thread_arg_t arg)
{
    int fd = -1;
    char recv_buf[UDP_RECV_MAX];
    fd_set readfds;
    struct timeval t;
    struct sockaddr_in from;
    socklen_t from_len;
    int ret;
    (void) arg;

    udp_disc_running = true;
    udp_log("UDP discover thread start, port=%d", UDP_DISCOVER_PORT);

    while (!udp_disc_should_exit) {
        if (fd < 0) {
            fd = UdpDiscOpenSocket();
            if (fd < 0) {
                mico_thread_msleep(1000);
                continue;
            }
            udp_log("UDP discover bound :%d", UDP_DISCOVER_PORT);
        }

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        t.tv_sec = 0;
        t.tv_usec = 500000; /* 500ms：便于退出/重连检查 */

        ret = select(fd + 1, &readfds, NULL, NULL, &t);
        if (ret < 0) {
            udp_log("UDP discover select err, rebind");
            UdpDiscCloseSocket(&fd);
            mico_thread_msleep(200);
            continue;
        }
        if (ret == 0) {
            continue;
        }

        from_len = sizeof(from);
        memset(&from, 0, sizeof(from));
        ret = (int) recvfrom(fd, recv_buf, sizeof(recv_buf) - 1, 0,
                             (struct sockaddr *) &from, &from_len);
        if (ret < 0) {
            /* WiFi 切换/接口 down 时常见，关掉重 bind */
            udp_log("UDP discover recv err, rebind");
            UdpDiscCloseSocket(&fd);
            mico_thread_msleep(200);
            continue;
        }
        if (ret == 0) continue;

        recv_buf[ret] = 0;

        if (UdpDiscIsReport(recv_buf, ret)) {
            UdpDiscSendReport(fd, &from);
        } else if (ZcHandleCommand(recv_buf)) {
            /* ZControl 控制/查询：单播回 10181 */
            static char state[UDP_SEND_MAX];
            struct sockaddr_in to;
            int n, len;

            n = ZcBuildStateJson(state, (int) sizeof(state));
            if (n > 0) {
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = htons(UDP_DISCOVER_PEER_PORT);
                to.sin_addr = from.sin_addr;
                len = (int) sendto(fd, state, (size_t) n, 0,
                                   (struct sockaddr *) &to, sizeof(to));
                udp_log("UDP ZC reply len=%d/%d", len, n);
            }
        }
    }

    UdpDiscCloseSocket(&fd);
    udp_disc_running = false;
    udp_log("UDP discover thread exit");
    mico_rtos_delete_thread(NULL);
}

void UdpDiscoverInit(void)
{
    OSStatus err;

    if (udp_disc_running) return;

    udp_disc_should_exit = false;
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "udp_disc",
                                  UdpDiscoverThread, 0x1000, 0);
    if (err != kNoErr) {
        udp_log("ERROR: UDP discover thread create fail %d", (int) err);
    }
}

void UdpDiscoverDeinit(void)
{
    udp_disc_should_exit = true;
}
