#ifndef __USER_MQTT_CLIENT_H_
#define __USER_MQTT_CLIENT_H_


#include "mico.h"

#define MQTT_CLIENT_KEEPALIVE   30
#define MQTT_CLIENT_SUB_TOPIC1  "device/ztc1/set"
#define MQTT_CLIENT_PUB_TOPIC   "device/ztc1/%s/state"
/* 遗嘱/在线状态主题。LWT 断联时 broker 自动发 "offline"，
 * 前端和 HA 据此把 UI 置灰，区分"设备离线"和"命令丢了"。 */
#define MQTT_CLIENT_AVAIL_TOPIC "device/ztc1/%s/availability"
#define MQTT_CLIENT_AVAIL_ON    "online"
#define MQTT_CLIENT_AVAIL_OFF   "offline"
/* 周期性全量状态重发周期（秒），是丢包自愈的根本手段 */
#define MQTT_STATE_REPUBLISH_SEC  30

#define MQTT_CMD_TIMEOUT        5000  // 5s
#define MQTT_YIELD_TMIE         5000  // 5s
/* select 轮询周期：同时也是 UserMqttDeInit 生效的最大延迟 */
#define MQTT_POLL_TMIE          500
#define MQTT_DEINIT_TIMEOUT_MS  8000

#define MAX_MQTT_TOPIC_SIZE         (512)
#define MAX_MQTT_DATA_SIZE          (2048)
#define MAX_MQTT_SEND_QUEUE_SIZE    (10)

#define MQTT_SERVER      user_config->mqtt_ip
#define MQTT_SERVER_PORT user_config->mqtt_port
#define MQTT_SERVER_USR  user_config->mqtt_user
#define MQTT_SERVER_PWD  user_config->mqtt_password
#define MQTT_REPORT_FREQ  user_config->mqtt_report_freq
#define MQTT_LED_ENABLED  user_config->power_led_enabled

extern OSStatus UserMqttInit(void);
/* 只发出退出请求，不阻塞调用线程（可在定时器回调里调用） */
extern OSStatus UserMqttDeInit(void);
/* 发出退出请求并等待线程真正结束（只能在普通线程里调用） */
extern OSStatus UserMqttDeInitWait(void);

extern OSStatus UserMqttSend(char *arg);

extern bool UserMqttIsConnect(void);

/* retained=0/1；qos=0/1。状态类发布建议 qos=1（见 UserMqttSendTopicQos 注释） */
extern OSStatus UserMqttSendTopic(char *topic, char *arg, char retained);
extern OSStatus UserMqttSendTopicQos(char *topic, char *arg, char retained, char qos);
/* 发布在线/离线状态（retained），供前端/HA 判定设备可达性 */
extern void UserMqttSendAvailability(char online);

extern OSStatus UserMqttSendSocketState(char socket_id);

extern OSStatus UserMqttSendLedState(void);

extern OSStatus UserMqttSendChildLockState(void);

extern OSStatus UserMqttSendTotalSocketState(void);

extern void UserMqttHassAuto(char socket_id);

extern void UserMqttHassPower(void);

extern void UserMqttHassAutoPower(void);

extern void UserMqttHassAutoLed(void);

extern void UserMqttHassAutoChildLock(void);

extern void UserMqttHassAutoTotalSocket(void);

extern void registerMqttEvents(void);

extern void UserMqttHassAutoRebootButton(void);

/* 全量重发 6 路 socket 状态 + 总开关/LED/童锁 + availability（丢包自愈） */
extern void UserMqttRepublishAllStates(void);

#endif
