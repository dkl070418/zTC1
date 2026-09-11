#include "user_wifi.h"

#include "main.h"
#include "mico_socket.h"
#include "user_gpio.h"
#include "user_power.h"
#include "http_server/web_log.h"
#include "mqtt_server/user_mqtt_client.h"

char wifi_status = WIFI_STATE_NOCONNECT;

mico_timer_t wifi_led_timer;
IpStatus ip_status = { 0, ZZ_AP_LOCAL_IP, ZZ_AP_LOCAL_IP, ZZ_AP_NET_MASK };

/* ------------------------------------------------------------------------- *
 * WiFi / SoftAP 状态机
 *
 * mico_notify_WIFI_STATUS_CHANGED 的回调运行在 WiFi 协议栈线程上。在该上下文里
 * 直接写 flash(mico_system_context_update) 或再次调用 micoWlanStart /
 * micoWlanSuspendSoftAP 会重入协议栈，导致协议栈死锁 -> 看门狗复位，表现为
 * "WiFi 不可用后设备反复重启"。因此这些操作全部投递到 worker 线程执行。
 * ------------------------------------------------------------------------- */
typedef enum {
    WIFI_WORK_NONE = 0,
    WIFI_WORK_STATION_UP,
    WIFI_WORK_STATION_DOWN,
} wifi_work_t;

static mico_worker_thread_t wifi_worker_thread;
static bool wifi_worker_ready = false;

static mico_mutex_t wifi_ap_mutex;
static bool wifi_ap_mutex_ready = false;
static volatile bool softap_active = false;

static void WifiApLock(void)
{
    if (wifi_ap_mutex_ready) mico_rtos_lock_mutex(&wifi_ap_mutex);
}

static void WifiApUnlock(void)
{
    if (wifi_ap_mutex_ready) mico_rtos_unlock_mutex(&wifi_ap_mutex);
}

/* force: true 强制重建AP；false 时若AP已开启则跳过 */
static void ApStart(bool force);

/* ------------------------------------------------------------------------- *
 * 扫描结果：WiFi 线程生产，HTTP 线程消费，用互斥量保护，指针所有权单一
 * ------------------------------------------------------------------------- */
static mico_mutex_t wifi_scan_mutex;
static bool wifi_scan_mutex_ready = false;
static volatile bool scan_in_flight = false;
static volatile bool scan_ready = false;
static char *scan_result = NULL;
static uint32_t scan_start_ms = 0;

static void ScanLock(void)
{
    if (wifi_scan_mutex_ready) mico_rtos_lock_mutex(&wifi_scan_mutex);
}

static void ScanUnlock(void)
{
    if (wifi_scan_mutex_ready) mico_rtos_unlock_mutex(&wifi_scan_mutex);
}

/* 释放旧结果，调用者必须持有 ScanLock */
static void ScanFreeResultLocked(void)
{
    if (scan_result) {
        free(scan_result);
        scan_result = NULL;
    }
    scan_ready = false;
}

/* 挂上新结果并接管其所有权；json 为 NULL 表示本次扫描无产出 */
static void ScanPublishResult(char *json)
{
    ScanLock();
    scan_in_flight = false;
    ScanFreeResultLocked();
    if (json) {
        scan_result = json;
        scan_ready = true;
    }
    ScanUnlock();
}

static char *ScanAllocEmptyResult(void)
{
    char *json = (char *) malloc(sizeof(WIFI_SCAN_EMPTY_JSON));
    if (json) strcpy(json, WIFI_SCAN_EMPTY_JSON);
    return json;
}

//wifi已连接获取到IP地址回调
static void WifiGetIpCallback(IPStatusTypedef *pnet, void * arg)
{
    if (pnet == NULL) return;

    strncpy(ip_status.ip, pnet->ip, sizeof(ip_status.ip) - 1);
    ip_status.ip[sizeof(ip_status.ip) - 1] = 0;
    strncpy(ip_status.gateway, pnet->gate, sizeof(ip_status.gateway) - 1);
    ip_status.gateway[sizeof(ip_status.gateway) - 1] = 0;
    strncpy(ip_status.mask, pnet->mask, sizeof(ip_status.mask) - 1);
    ip_status.mask[sizeof(ip_status.mask) - 1] = 0;

    wifi_log("got IP:%s", ip_status.ip);
    wifi_status = WIFI_STATE_CONNECTED;
}

/* 在 worker 线程里执行被延迟下来的 WiFi 事件 */
static OSStatus WifiWorkHandler(void *arg)
{
    switch ((int) (intptr_t) arg) {
        case WIFI_WORK_STATION_UP:
            ApStop();
            ip_status.mode = 1;
            mico_system_context_update(sys_config);
            break;

        case WIFI_WORK_STATION_DOWN:
            mico_system_context_update(sys_config);
            ApStart(false);
            break;

        default:
            break;
    }
    return kNoErr;
}

static void WifiPostWork(wifi_work_t work)
{
    if (!wifi_worker_ready) {
        /* worker 尚未创建（仅启动早期可能），退回同步执行，保证事件不丢 */
        WifiWorkHandler((void *) (intptr_t) work);
        return;
    }

    if (mico_rtos_send_asynchronous_event(&wifi_worker_thread, WifiWorkHandler,
                                          (void *) (intptr_t) work) != kNoErr) {
        wifi_log("post wifi work[%d] failed", (int) work);
    }
}

//wifi连接状态改变回调
static void WifiStatusCallback(WiFiEvent status, void* arg)
{
    if (status == NOTIFY_STATION_UP) //wifi连接成功
    {
        sys_config->micoSystemConfig.reserved = status;
        WifiPostWork(WIFI_WORK_STATION_UP);
    }
    else if (status == NOTIFY_STATION_DOWN) //wifi断开
    {
        sys_config->micoSystemConfig.reserved = status;
        wifi_status = WIFI_STATE_NOCONNECT;
        if (!mico_rtos_is_timer_running(&wifi_led_timer))
        {
            mico_rtos_start_timer(&wifi_led_timer);
        }
        WifiPostWork(WIFI_WORK_STATION_DOWN);
    }
    else if (status == NOTIFY_AP_UP)
    {
        ip_status.mode = 0;
        WifiApLock();
        softap_active = true;
        WifiApUnlock();
    }
    else if (status == NOTIFY_AP_DOWN)
    {
        WifiApLock();
        softap_active = false;
        WifiApUnlock();
    }
}

//wifi扫描结果回调
void WifiScanCallback(ScanResult_adv* scan_ret, void* arg)
{
    int i, total, kept = 0;
    int keep[WIFI_SCAN_MAX_APS];
    size_t ssid_bytes = 0, sec_bytes = 0, json_alloc;
    char *ssids = NULL, *secs = NULL, *json = NULL;
    char *p1, *p2;

    /* ApNum 是 signed char，驱动报告 >127 个AP时会翻成负数 */
    total = (scan_ret != NULL && scan_ret->ApList != NULL)
            ? (int) (unsigned char) scan_ret->ApNum : 0;
    if (total > WIFI_SCAN_MAX_APS) total = WIFI_SCAN_MAX_APS;

    wifi_log("wifi_scan_callback ApNum[%d]", total);

    /* 第一遍：过滤 + 精确计算所需长度。scan_ret 只在回调期间有效，必须当场拷出 */
    for (i = 0; i < total; i++) {
        char ssid[33];
        int len;

        memcpy(ssid, scan_ret->ApList[i].ssid, 32);
        ssid[32] = 0;

        /* 排除隐藏SSID，以及含引号/反斜杠会破坏返回JSON的SSID */
        if (ssid[0] == 0 || strchr(ssid, '\'') || strchr(ssid, '"') || strchr(ssid, '\\'))
            continue;

        len = (int) strlen(ssid);
        ssid_bytes += (size_t) len + 3;   /* 'ssid', */
        sec_bytes  += 3;                  /* 'n', */
        keep[kept++] = i;
    }

    if (kept == 0) {
        ScanPublishResult(ScanAllocEmptyResult());
        return;
    }

    ssids = (char *) malloc(ssid_bytes + 1);
    secs  = (char *) malloc(sec_bytes + 1);
    json_alloc = ssid_bytes + sec_bytes + 48;  /* 48 > "{'success':1,'ssids':[],'secs':[]}" */
    json = (char *) malloc(json_alloc);

    if (ssids == NULL || secs == NULL || json == NULL) {
        wifi_log("scan result out of memory");
        if (ssids) free(ssids);
        if (secs) free(secs);
        if (json) free(json);
        ScanPublishResult(ScanAllocEmptyResult());
        return;
    }

    p1 = ssids;
    p2 = secs;
    for (i = 0; i < kept; i++) {
        char ssid[33];
        int n;

        /* 必须重新拷出并补NUL：驱动里的 ssid[32] 不保证以NUL结尾，
         * 直接当 "%s" 用会读过界，写出的长度也会超过第一遍算好的预算 */
        memcpy(ssid, scan_ret->ApList[keep[i]].ssid, 32);
        ssid[32] = 0;

        n = sprintf(p1, "'%s',", ssid);
        p1 += n;
        n = sprintf(p2, "%d,", (int) scan_ret->ApList[keep[i]].security % 10);
        p2 += n;
    }
    p1[-1] = 0;   /* 去掉末尾逗号，kept>0 保证 p1>ssids */
    p2[-1] = 0;

    snprintf(json, json_alloc, WIFI_SCAN_RESULT_JSON, 1, ssids, secs);
    json[json_alloc - 1] = 0;

    free(ssids);
    free(secs);

    ScanPublishResult(json);
}

/* 100ms定时器回调 */
static void WifiLedTimerCallback(void* arg)
{
    static unsigned int num = 0;
    num++;

    switch (wifi_status)
    {
        case WIFI_STATE_FAIL:
            wifi_log("wifi connect fail");
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            break;
        case WIFI_STATE_NOCONNECT:
            //wifi_connect_sys_config();
            break;
        case WIFI_STATE_CONNECTING:
            num = 0;
            UserLedSet(-1);
            break;
        case WIFI_STATE_CONNECTED:
            if (!(MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)){
                UserMqttInit();
            }
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            if (RelayOut()&&user_config->power_led_enabled)
                UserLedSet(1);
            else
                UserLedSet(0);
            break;
    }
}

void WifiConnect(char* wifi_ssid, char* wifi_key)
{
    network_InitTypeDef_st wNetConfig;

    if (wifi_ssid == NULL || wifi_ssid[0] == 0) {
        wifi_log("WifiConnect: empty ssid, ignored");
        return;
    }
    if (wifi_key == NULL) wifi_key = (char *) "";

    wifi_log("WifiConnect wifi_ssid[%s] wifi_key[******]", wifi_ssid);

    memset(&wNetConfig, 0, sizeof(network_InitTypeDef_st));
    wNetConfig.wifi_mode = Station;
    /* 注意：这里绝不能用 snprintf(dst, n, src)，src 是用户可控字符串，
     * 含 %n/%s 会造成格式化字符串漏洞。 */
    strncpy((char *) wNetConfig.wifi_ssid, wifi_ssid, sizeof(wNetConfig.wifi_ssid) - 1);
    strncpy((char *) wNetConfig.wifi_key, wifi_key, sizeof(wNetConfig.wifi_key) - 1);
    wNetConfig.dhcpMode = DHCP_Client;
    wNetConfig.wifi_retry_interval = 6000;
    micoWlanStart(&wNetConfig);
    /* 同上：切换 Station 模式也会重排引脚复用，必须重新武装功耗脉冲中断 */
    PowerReinit();

    /* 保存wifi及密码到Flash，长度裁剪到flash字段大小，避免溢出相邻字段 */
    strncpy(sys_config->micoSystemConfig.ssid, wNetConfig.wifi_ssid,
            sizeof(sys_config->micoSystemConfig.ssid) - 1);
    strncpy(sys_config->micoSystemConfig.user_key, wNetConfig.wifi_key,
            sizeof(sys_config->micoSystemConfig.user_key) - 1);
    sys_config->micoSystemConfig.user_keyLength =
            (int) strlen(sys_config->micoSystemConfig.user_key);
    mico_system_context_update(sys_config);
    wifi_status = WIFI_STATE_NOCONNECT;
}

void WifiInit(void)
{
    if (!wifi_ap_mutex_ready) {
        mico_rtos_init_mutex(&wifi_ap_mutex);
        wifi_ap_mutex_ready = true;
    }
    if (!wifi_scan_mutex_ready) {
        mico_rtos_init_mutex(&wifi_scan_mutex);
        wifi_scan_mutex_ready = true;
    }
    if (!wifi_worker_ready) {
        if (mico_rtos_create_worker_thread(&wifi_worker_thread, MICO_APPLICATION_PRIORITY,
                                           0x1000, 8) != kNoErr) {
            wifi_log("create wifi worker failed, wifi events run inline");
        } else {
            wifi_worker_ready = true;
        }
    }

    //wifi状态下led闪烁定时器初始化
    mico_rtos_init_timer(&wifi_led_timer, 100, (void*)WifiLedTimerCallback, NULL);
    //wifi已连接获取到IP地址 回调
    mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void*)WifiGetIpCallback, NULL);
    //wifi连接状态改变回调
    mico_system_notify_register(mico_notify_WIFI_STATUS_CHANGED, (void*)WifiStatusCallback, NULL);
    //wifi扫描结果回调
    mico_system_notify_register(mico_notify_WIFI_SCAN_ADV_COMPLETED, (void*)WifiScanCallback, NULL);

    //启动定时器开始进行wifi连接
    if (!mico_rtos_is_timer_running(&wifi_led_timer)) mico_rtos_start_timer(&wifi_led_timer);
}

void WifiDeinit(void)
{
    mico_rtos_stop_timer(&wifi_led_timer);

    ScanLock();
    ScanFreeResultLocked();
    scan_in_flight = false;
    ScanUnlock();

    WifiApLock();
    softap_active = false;
    WifiApUnlock();

    if (wifi_worker_ready) {
        mico_rtos_delete_worker_thread(&wifi_worker_thread);
        wifi_worker_ready = false;
    }
}

void ApConfig(char* name, char* key)
{
    if (name != NULL) {
        memset(user_config->ap_name, 0, sizeof(user_config->ap_name));
        strncpy(user_config->ap_name, name, sizeof(user_config->ap_name) - 1);
    }
    if (key != NULL) {
        memset(user_config->ap_key, 0, sizeof(user_config->ap_key));
        strncpy(user_config->ap_key, key, sizeof(user_config->ap_key) - 1);
    }
    wifi_log("ApConfig ap_name[%s] ap_key[******]", user_config->ap_name);
    micoWlanSuspendStation();
    ApStart(true);
    mico_system_context_update(sys_config);
}

/* 调用者必须持有 wifi_ap_mutex */
static void ApStartLocked(bool force)
{
    network_InitTypeDef_st wNetConfig;
    const char *ssid;
    const char *key;

    if (softap_active && !force) {
        wifi_log("ap already up, skip start");
        return;
    }

    ssid = user_config->ap_name;
    /* SSID 为空或含不可打印字符时回退到默认名，否则会起一个隐藏/无法连接的AP，
     * 用户既连不上也进不了配网页面，只能断电重试。 */
    if (ssid[0] < 0x20 || ssid[0] > 0x7E) {
        wifi_log("ap_name invalid, fallback to default name");
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        ssid = user_config->ap_name;
    }

    key = user_config->ap_key;
    if (key[0] != 0 && (key[0] < 0x20 || key[0] > 0x7E)) {
        wifi_log("ap_key invalid, cleared");
        user_config->ap_key[0] = 0;
        key = user_config->ap_key;
    }

    memset(&wNetConfig, 0x00, sizeof(network_InitTypeDef_st));
    strncpy((char *) wNetConfig.wifi_ssid, ssid, sizeof(wNetConfig.wifi_ssid) - 1);
    strncpy((char *) wNetConfig.wifi_key, key, sizeof(wNetConfig.wifi_key) - 1);
    strncpy((char *) wNetConfig.local_ip_addr, ZZ_AP_LOCAL_IP,
            sizeof(wNetConfig.local_ip_addr) - 1);
    strncpy((char *) wNetConfig.net_mask, ZZ_AP_NET_MASK, sizeof(wNetConfig.net_mask) - 1);
    strncpy((char *) wNetConfig.dnsServer_ip_addr, ZZ_AP_DNS_SERVER,
            sizeof(wNetConfig.dnsServer_ip_addr) - 1);
    wNetConfig.wifi_mode = Soft_AP;
    wNetConfig.dhcpMode = DHCP_Server;
    wNetConfig.wifi_retry_interval = 100;

    if (micoWlanStart(&wNetConfig) != kNoErr) {
        wifi_log("ApInit micoWlanStart failed");
        return;
    }
    /* micoWlanStart 会让 MoC 重新初始化引脚复用，把功耗脉冲输入的中断冲掉 */
    PowerReinit();

    softap_active = true;
    ip_status.mode = 0;
    wifi_log("ApInit ssid[%s] key[******]", wNetConfig.wifi_ssid);
}

/* force=true: 重建AP并使用默认名(use_default)；force=false: AP已开着就什么都不做 */
static void ApStart(bool force)
{
    WifiApLock();
    ApStartLocked(force);
    WifiApUnlock();
}

void ApInit(bool use_default)
{
    if (use_default)
    {
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        sprintf(user_config->ap_key, "%s", ZZ_AP_KEY);
        wifi_log("ApInit use_default[true] key[]");
        ApStart(true);
    }
    else
    {
        ApStart(false);
    }
}

void ApStop(void)
{
    OSStatus s;

    WifiApLock();
    s = micoWlanSuspendSoftAP(); //关闭AP
    if (s != kNoErr)
    {
        wifi_log("close ap error[%d]", s);
    }
    softap_active = false;
    WifiApUnlock();
}

void WifiScanStart(void)
{
    uint32_t now = mico_rtos_get_time();

    ScanLock();
    if (scan_in_flight && (now - scan_start_ms) < WIFI_SCAN_TIMEOUT_MS) {
        ScanUnlock();
        wifi_log("scan already in flight, ignore request");
        return;
    }
    /* 上一次的残留结果直接丢弃，避免HTTP侧拿到过期列表 */
    ScanFreeResultLocked();
    scan_in_flight = true;
    scan_start_ms = now;
    ScanUnlock();

    /* micoWlanStartScanAdv 是 void 宏，无法取返回值；失败时靠超时兜底解锁 */
    micoWlanStartScanAdv();
}

bool WifiScanIsBusy(void)
{
    bool busy;

    ScanLock();
    busy = scan_in_flight && !scan_ready;
    if (busy && (mico_rtos_get_time() - scan_start_ms) >= WIFI_SCAN_TIMEOUT_MS) {
        /* 驱动一直没回调，强制解锁，否则以后再也无法发起扫描 */
        scan_in_flight = false;
        busy = false;
    }
    ScanUnlock();

    return busy;
}

char* WifiScanConsumeResult(void)
{
    char *out = NULL;

    ScanLock();
    if (scan_ready && scan_result) {
        out = scan_result;
        scan_result = NULL;
        scan_ready = false;
    }
    ScanUnlock();

    return out;
}
