#ifndef __MAIN_H_
#define __MAIN_H_
#include <stddef.h>

#include "mico.h"
#include "micokit_ext.h"
#include "timed_task/timed_task.h"

/*
 * 杩欎簺瀹忓睍寮€鎴愪袱鏉¤鍙ワ紙custom_log 璧颁覆鍙ｏ紝web_log 璧扮綉椤垫棩蹇楅〉锛夈€?
 * 鍘熸潵娌℃湁鐢?do{}while(0) 鍖呰捣鏉ワ紝鎵€浠?
 *     if (cond) tc1_log("...");
 * 浼氬睍寮€鎴?
 *     if (cond) custom_log(...);
 *     web_log(...);            <- 鏃犳潯浠舵墽琛岋紒
 * 缁撴灉灏辨槸涓插彛鏄鐨勶紝浣嗙綉椤垫棩蹇楅〉閲屽啋鍑轰竴鍫嗗亣鐨?"ERROR"锛堜緥濡?
 * user_rtc.c 閭ｆ潯 err:0 鐨?ERROR1锛夈€傚叏閮ㄦ敼鎴愬崟璇彞瀹忋€?
 */
#define app_log(M, ...)   do { custom_log("APP", M, ##__VA_ARGS__);   web_log("APP", M, ##__VA_ARGS__);   } while (0)
#define key_log(M, ...)   do { custom_log("KEY", M, ##__VA_ARGS__);   web_log("KEY", M, ##__VA_ARGS__);   } while (0)
#define ota_log(M, ...)   do { custom_log("OTA", M, ##__VA_ARGS__);   web_log("OTA", M, ##__VA_ARGS__);   } while (0)
#define rtc_log(M, ...)   do { custom_log("RTC", M, ##__VA_ARGS__);   web_log("RTC", M, ##__VA_ARGS__);   } while (0)
#define tc1_log(M, ...)   do { custom_log("TC1", M, ##__VA_ARGS__);   web_log("TC1", M, ##__VA_ARGS__);   } while (0)
#define task_log(M, ...)  do { custom_log("TASK", M, ##__VA_ARGS__);  web_log("TASK", M, ##__VA_ARGS__);  } while (0)
#define http_log(M, ...)  do { custom_log("HTTP", M, ##__VA_ARGS__);  web_log("HTTP", M, ##__VA_ARGS__);  } while (0)
#define mqtt_log(M, ...)  do { custom_log("MQTT", M, ##__VA_ARGS__);  web_log("MQTT", M, ##__VA_ARGS__);  } while (0)
#define wifi_log(M, ...)  do { custom_log("WIFI", M, ##__VA_ARGS__);  web_log("WIFI", M, ##__VA_ARGS__);  } while (0)
#define power_log(M, ...) do { custom_log("POWER", M, ##__VA_ARGS__); web_log("POWER", M, ##__VA_ARGS__); } while (0)
#define udp_log(M, ...)   do { custom_log("UDP", M, ##__VA_ARGS__);   web_log("UDP", M, ##__VA_ARGS__);   } while (0)

#define VERSION "v2.5.2-recovery"

#define TYPE 1
#define TYPE_NAME "TC1"

#define ZTC1_NAME "TC1-%s"

#define USER_CONFIG_VERSION 9
#define SETTING_MQTT_STRING_LENGTH_MAX 32 //蹇呴』4瀛楄妭瀵归綈銆?

#define SOCKET_NAME_LENGTH   64
#define SOCKET_NUM           6  //鎻掑骇鏁伴噺

#define Led    MICO_GPIO_5
#define Button MICO_GPIO_23
#define POWER  MICO_GPIO_15

#define Relay_ON  1
#define Relay_OFF 0
#define Relay_TOGGLE -1

#define Relay_0   MICO_GPIO_6
#define Relay_1   MICO_GPIO_8
#define Relay_2   MICO_GPIO_10
#define Relay_3   MICO_GPIO_7
#define Relay_4   MICO_GPIO_9
#define Relay_5   MICO_GPIO_18
#define Relay_NUM SOCKET_NUM

#define MAX_TASK_NUM 128

//鐢ㄦ埛淇濆瓨鍙傛暟缁撴瀯浣?
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //璁板綍褰撳墠寮€鍏?
    char user[maxNameLen];
    /* 原为 WiFiEvent last_wifi_status —— 全项目零引用（声明后从未读写）。
     * WiFiEvent 即 notify_wlan_t，未开 -short-enums 故为 4 字节 int，
     * 这里用同尺寸同偏移的 int 顶替：user_config_t 总大小和所有字段偏移
     * 完全不变，flash 里已有的配置不会因 CRC 失配或版本检查而被重置。
     * 存功耗标定值：1 个 CF 脉冲代表多少 µWh。 */
    int power_pulse_uwh;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    pTimedTask task_top;
    struct TimedTask timed_tasks[MAX_TASK_NUM];
} user_config_t;

/*
 * 编译期锁死"user_config_t 布局未变"。
 *
 * power_pulse_uwh 顶替了原来零引用的 WiFiEvent last_wifi_status（同为 4 字节 int），
 * 所以整个结构体大小和后续所有字段的偏移都必须和替换前一模一样 —— 只有这样
 * flash 里已有的配置才不会因为 sizeof 变化导致 CRC 失配而被重置。
 * 如果有人在这两者之间插入/删除字段，下面两条会直接编译失败。
 */
_Static_assert(sizeof(((user_config_t *) 0)->power_pulse_uwh) == 4,
               "power_pulse_uwh must stay 4 bytes to preserve user_config_t layout");
_Static_assert(offsetof(user_config_t, ap_name) == offsetof(user_config_t, power_pulse_uwh) + 4,
               "ap_name must stay immediately after power_pulse_uwh (layout change => flash config reset)");
_Static_assert(offsetof(user_config_t, power_pulse_uwh) >= offsetof(user_config_t, user) + maxNameLen
               && offsetof(user_config_t, power_pulse_uwh) <= offsetof(user_config_t, user) + maxNameLen + 3,
               "power_pulse_uwh must stay right after user[] (allowing only the alignment padding "
               "that the original 4-byte WiFiEvent field already required)");

extern char rtc_init;
extern uint32_t total_time;
extern char str_mac[16];
extern system_config_t* sys_config;
extern user_config_t* user_config;
extern mico_gpio_t Relay[Relay_NUM];
extern int childLockEnabled;


#endif
