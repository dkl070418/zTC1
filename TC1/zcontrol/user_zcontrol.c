/*
 * ZControl JSON 控制桥（阶段 C）
 *
 * 与 HA 文本协议并存：
 *   HA/MQTT 仍是 set socket <mac> <idx> <on>
 *   ZControl 是 {"mac":..,"plug_N":{"on":0|1}}
 * 两边改继电器后都应回一份 ZControl 状态 JSON（MQTT topic device/ztc1/<mac>/state）。
 *
 * 刻意不用 json_c：命令字段固定且浅，手写扫描更省堆、少一次解析分配。
 */
#include "main.h"
#include "user_gpio.h"
#include "user_power.h"
#include "user_zcontrol.h"
#include "mqtt_server/user_mqtt_client.h"
#include "http_server/web_log.h"

#include <string.h>
#include <stdio.h>

#define ZC_STATE_JSON_MAX 1024

static int ZcHexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* json 里 "mac":"xxxxxxxxxxxx" 与本机 str_mac 十六进制等价（忽略大小写） */
static bool ZcMacMatches(const char *json)
{
    const char *p;
    int i;

    if (json == NULL) return false;
    p = strstr(json, "\"mac\"");
    if (p == NULL) return false;
    p = strchr(p, ':');
    if (p == NULL) return false;
    p = strchr(p, '"');
    if (p == NULL) return false;
    p++;
    for (i = 0; i < 12; i++) {
        if (ZcHexVal(p[i]) != ZcHexVal(str_mac[i])) return false;
    }
    return true;
}

/* 从 "plug_N" ... "on": 0|1|null 取开关值；null 或其它返回 -1 */
static int ZcParsePlugOn(const char *json, int idx)
{
    char key[16];
    const char *p;
    int n;

    n = snprintf(key, sizeof(key), "\"plug_%d\"", idx);
    if (n <= 0) return -1;
    p = strstr(json, key);
    if (p == NULL) return -1;
    p = strstr(p, "\"on\"");
    if (p == NULL) return -1;
    p = strchr(p, ':');
    if (p == NULL) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n') return -1;          /* null */
    if (*p == '0') return 0;
    if (*p == '1') return 1;
    return -1;
}

static void ZcSanitizeName(char *dst, int dst_len, const char *src)
{
    int i;

    if (dst_len <= 0) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
    for (i = 0; dst[i]; i++) {
        if (dst[i] == '"' || dst[i] == '\\') dst[i] = '-';
    }
}

int ZcBuildStateJson(char *buf, int buflen)
{
    char name[SOCKET_NAME_LENGTH];
    char mac_lc[13];
    int i, n, off = 0;

    if (buf == NULL || buflen < 64) return 0;

    for (i = 0; i < 12; i++) {
        char c = str_mac[i];
        mac_lc[i] = (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
    }
    mac_lc[12] = 0;

    n = snprintf(buf, (size_t) buflen,
                 /* V1 激活码 DRM 在 V2 无意义；恒报已激活，避免 ZControl 一直显示「未激活」 */
                 "{\"mac\":\"%s\",\"version\":\"%s\",\"lock\":true,\"child_lock\":%d,\"led_lock\":%d,"
                 "\"power\":\"%.1f\",\"total_time\":%lu",
                 mac_lc, VERSION, childLockEnabled ? 1 : 0,
                 user_config->power_led_enabled ? 1 : 0,
                 (double) (real_time_power / 10.0f), (unsigned long) total_time);
    if (n < 0 || n >= buflen) return 0;
    off = n;

    for (i = 0; i < SOCKET_NUM; i++) {
        ZcSanitizeName(name, (int) sizeof(name), user_config->socket_names[i]);
        n = snprintf(buf + off, (size_t) (buflen - off),
                     ",\"plug_%d\":{\"on\":%d,\"setting\":{\"name\":\"%s\"}}",
                     i, user_config->socket_status[i] ? 1 : 0, name);
        if (n < 0 || off + n >= buflen) break;
        off += n;
    }

    if (off + 2 >= buflen) return 0;
    buf[off++] = '}';
    buf[off] = 0;
    return off;
}

/* 解析并执行 ZControl 命令；不是本机命令返回 false */
bool ZcHandleCommand(const char *json)
{
    bool changed = false;
    int i, on;

    if (json == NULL || json[0] != '{') return false;
    if (!ZcMacMatches(json)) return false;

    for (i = 0; i < SOCKET_NUM; i++) {
        on = ZcParsePlugOn(json, i);
        if (on < 0) continue;
        if (user_config->socket_status[i] == (char) on) continue;
        UserRelaySet(i, (char) on);
        changed = true;
    }

    if (changed) {
        mico_system_context_update(sys_config);
        for (i = 0; i < SOCKET_NUM; i++) {
            UserMqttSendSocketState((char) i);
        }
        UserMqttSendTotalSocketState();
        udp_log("ZC set sockets=%s", GetSocketStatus());
    }

    /* 查询(全 null)也返回 true，调用方负责回状态包 */
    return true;
}

void ZcPublishState(void)
{
    static char state[ZC_STATE_JSON_MAX];
    int n;

    if (!UserMqttIsConnect()) return;

    n = ZcBuildStateJson(state, (int) sizeof(state));
    if (n <= 0) return;
    /* topic_state 即 device/ztc1/<mac>/state，ZControl 订阅该主题 */
    UserMqttSendTopic(topic_state, state, 1);
}
