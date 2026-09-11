#ifndef __USER_WIFI_H_
#define __USER_WIFI_H_

#include "mico.h"
#include "mico_wlan.h"
#include "micokit_ext.h"

enum
{
   WIFI_STATE_FAIL,
   WIFI_STATE_NOCONNECT,
   WIFI_STATE_CONNECTING,
   WIFI_STATE_CONNECTED,
};

#define ZZ_AP_NAME       "TC1-AP-%s"
#define ZZ_AP_KEY        ""
#define ZZ_AP_LOCAL_IP   "192.168.0.1"
#define ZZ_AP_DNS_SERVER "192.168.0.1"
#define ZZ_AP_NET_MASK   "255.255.255.0"

#define WIFI_SCAN_RESULT_JSON   "{'success':%d,'ssids':[%s],'secs':[%s]}"
#define WIFI_SCAN_EMPTY_JSON    "{'success':1,'ssids':[],'secs':[]}"
/* 单次扫描最多处理的AP数量，防止驱动返回异常数量时撑爆内存 */
#define WIFI_SCAN_MAX_APS       32
/* 扫描兜底超时，防止驱动不回调导致永久无法再次扫描 */
#define WIFI_SCAN_TIMEOUT_MS    20000

extern char wifi_status;

typedef struct {
    int  mode;  //0:AP, 1:Station
    char ip[16];
    char gateway[16];
    char mask[16];
} IpStatus;

typedef struct {
    char ssid[32];
    char bssid[6];
    char channel;
    wlan_sec_type_t security;
    int16_t rssi;
} ApInfo;

extern IpStatus ip_status;

extern void WifiInit(void);
extern void WifiDeinit(void);
/* use_default: true 强制使用默认名重建AP; false 仅在AP未开启时开启 */
extern void ApInit(bool use_default);
extern void ApStop(void);
extern void ApConfig(char* name, char* key);
extern void WifiConnect(char* wifi_ssid, char* wifi_key);

/* 扫描：WifiScanStart 防重入；结果由 WifiScanConsumeResult 取走(所有权转移给调用者) */
extern void WifiScanStart(void);
extern bool WifiScanIsBusy(void);
extern char* WifiScanConsumeResult(void);

#endif
