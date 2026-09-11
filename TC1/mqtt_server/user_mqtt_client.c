/**
 ******************************************************************************
 * @file    mqtt_client.c
 * @author  Eshen Wang
 * @version V1.0.0
 * @date    16-Nov-2015
 * @brief   MiCO application demonstrate a MQTT client.
 ******************************************************************************
 * @attention
 *
 * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
 * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
 * TIME. AS A RESULT, MXCHIP Inc. SHALL NOT BE HELD LIABLE FOR ANY
 * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
 * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
 * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
 *
 * <h2><center>&copy; COPYRIGHT 2014 MXCHIP Inc.</center></h2>
 ******************************************************************************
 */
#include "http_server/web_log.h"

#include "main.h"
#include "mico.h"
#include "MQTTClient.h"
#include "user_gpio.h"
#include "user_power.h"
#include "user_mqtt_client.h"

typedef struct {
    char topic[MAX_MQTT_TOPIC_SIZE];
    char qos;
    char retained;

    char data[MAX_MQTT_DATA_SIZE];
    uint32_t datalen;
} mqtt_recv_msg_t, *p_mqtt_recv_msg_t, mqtt_send_msg_t, *p_mqtt_send_msg_t;

static void MqttClientThread(mico_thread_arg_t arg);

static void MessageArrived(MessageData *md);

static OSStatus
MqttMsgPublish(Client *c, const char *topic, char qos, char retained, const unsigned char *msg,
               uint32_t msg_len);

OSStatus UserRecvHandler(void *arg);

void ProcessHaCmd(char *cmd);

bool isconnect = false;
mico_queue_t mqtt_msg_send_queue = NULL;

Client c;  // mqtt client object
Network n;  // socket network for mqtt client
volatile bool mqtt_thread_should_exit = false;
static volatile bool mqtt_thread_running = false;

/* 保护 Init/DeInit 与线程生命周期，避免"起了两个线程"或"删了别人在用的对象" */
static mico_mutex_t mqtt_lifecycle_mutex;
static bool mqtt_lifecycle_mutex_ready = false;

static mico_worker_thread_t mqtt_client_worker_thread; /* Worker thread to manage send/recv events */
static volatile bool mqtt_worker_running = false;
//static mico_timed_event_t mqtt_client_send_event;

char topic_state[MAX_MQTT_TOPIC_SIZE];
char topic_set[MAX_MQTT_TOPIC_SIZE];
char topic_availability[MAX_MQTT_TOPIC_SIZE];

mico_timer_t timer_handle;
static char timer_status = 0;
static bool timer_created = false;

void UserMqttTimerFunc(void *arg) {
    LinkStatusTypeDef LinkStatus;
    micoWlanGetLinkStatus(&LinkStatus);
    if (LinkStatus.is_connected != 1) {
        mico_stop_timer(&timer_handle);
        return;
    }
    if (mico_rtos_is_queue_empty(&mqtt_msg_send_queue)) {

        switch (timer_status) {
            case 0:
                UserMqttHassAutoLed();
                UserMqttHassAutoTotalSocket();
                UserMqttHassAutoChildLock();
                UserMqttHassAutoRebootButton();
                break;
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
                UserMqttHassAuto(timer_status);
                break;
            case 7:
                UserMqttHassAutoPower();
                break;
            default:
                mico_stop_timer(&timer_handle);
                break;
        }
        timer_status++;
    }
}

static void MqttLifecycleLock(void)
{
    if (mqtt_lifecycle_mutex_ready) mico_rtos_lock_mutex(&mqtt_lifecycle_mutex);
}

static void MqttLifecycleUnlock(void)
{
    if (mqtt_lifecycle_mutex_ready) mico_rtos_unlock_mutex(&mqtt_lifecycle_mutex);
}

/* 只请求退出，不等待：可以在定时器/中断一类不能阻塞的上下文里调用 */
OSStatus UserMqttDeInit(void) {
    mqtt_thread_should_exit = true;
    return kNoErr;
}

/* 请求退出并等待线程真正结束，之后 UserMqttInit 才能重新拉起连接 */
OSStatus UserMqttDeInitWait(void) {
    uint32_t waited = 0;

    UserMqttDeInit();

    while (mqtt_thread_running && waited < MQTT_DEINIT_TIMEOUT_MS) {
        mico_rtos_thread_msleep(50);
        waited += 50;
    }

    if (mqtt_thread_running) {
        mqtt_log("WARN: mqtt thread still running after %dms", (int) waited);
    } else {
        mqtt_log("mqtt thread stopped in %dms", (int) waited);
    }

    return kNoErr;
}

void clear_mqtt_msg_send_queue(void) {
    if (mqtt_msg_send_queue == NULL) {
        return;
    }
    void *msg = NULL;
    while (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) == false) {
        if (mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &msg, 0) == kNoErr) {
            if (msg) free(msg);  // 释放消息内存，避免泄漏
        }
    }
}

/* Application entrance */
OSStatus UserMqttInit(void) {
    OSStatus err = kNoErr;

    if (!mqtt_lifecycle_mutex_ready) {
        if (mico_rtos_init_mutex(&mqtt_lifecycle_mutex) == kNoErr)
            mqtt_lifecycle_mutex_ready = true;
    }

    MqttLifecycleLock();

    if (mqtt_thread_running) {
        /* 已经在跑，什么都不做（换服务器参数请先调用 UserMqttDeInitWait） */
        goto exit;
    }

    sprintf(topic_set, MQTT_CLIENT_SUB_TOPIC1);
    sprintf(topic_state, MQTT_CLIENT_PUB_TOPIC, str_mac);

    /* 队列一旦建立就常驻，不随线程销毁：其它线程可能正在往里投递消息 */
    if (mqtt_msg_send_queue == NULL) {
        err = mico_rtos_init_queue(&mqtt_msg_send_queue, "mqtt_msg_send_queue",
                                   sizeof(p_mqtt_send_msg_t),
                                   MAX_MQTT_SEND_QUEUE_SIZE);
        require_noerr_action(err, exit, mqtt_log("ERROR: create mqtt msg send queue err=%d.", err));
    }

    int mqtt_thread_stack_size = 0x2000;
    uint32_t mqtt_lib_version = MQTTClientLibVersion();mqtt_log(
            "MQTT client version: [%ld.%ld.%ld]",
            0xFF & (mqtt_lib_version >> 16), 0xFF & (mqtt_lib_version >> 8),
            0xFF & mqtt_lib_version);

    mqtt_thread_should_exit = false;
    /* handle 传 NULL：本线程靠 mqtt_thread_running 跟踪，从不 join，
     * 复用一个 handle 变量反而可能在线程重启的瞬间被写花 */
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY,
                                  "mqtt_client",
                                  (mico_thread_function_t) MqttClientThread,
                                  mqtt_thread_stack_size, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the mqtt client thread.");
    mqtt_thread_running = true;

    exit:
    if (kNoErr != err) mqtt_log("ERROR2, app thread exit err: %d kNoErr[%d]", err, kNoErr);
    MqttLifecycleUnlock();
    return err;
}

static OSStatus UserMqttClientRelease(Client *c, Network *n) {
    OSStatus err = kNoErr;

    if (c == NULL || n == NULL) return kParamErr;

    /*
     * 断开前主动补发 "offline"（retained, QoS1）。
     * 遇到的三种情形：
     *   1) keepalive 超时/掉电  -> 这里发不出去，但 broker 会因为会话超时
     *      自动代发遗嘱(MQTT_CLIENT_AVAIL_OFF)，结果一致
     *   2) 用户改 MQTT 参数/优雅退出 -> 这里发得出去，最及时
     *   3) WiFi 突然断 -> 和 1 一样靠遗嘱兜底
     * 注意必须同步直发（MQTTPublish），不能再走发送队列——队列此刻正被清空。
     */
    if (c->isconnected) {
        MQTTDisconnect(c);
        c->isconnected = 0;
    }

    if (c->buf) {
        free(c->buf);
        c->buf = NULL;
    }

    if (c->readbuf) {
        free(c->readbuf);
        c->readbuf = NULL;
    }

    if (n->disconnect) {
        n->disconnect(n);
    }

    if (MQTT_SUCCESS != MQTTClientDeinit(c)) {
        mqtt_log("MQTTClientDeinit failed!");
        err = kDeletedErr;
    }

    return err;
}

// publish msg to mqtt server
static OSStatus MqttMsgPublish(Client *c, const char *topic, char qos, char retained,
                               const unsigned char *msg,
                               uint32_t msg_len) {
    OSStatus err = kUnknownErr;
    int ret = 0;
    MQTTMessage publishData = MQTTMessage_publishData_initializer;

    require(topic && msg_len && msg, exit);

    // upload data qos0
    publishData.qos = (enum QoS) qos;
    publishData.retained = retained;
    publishData.payload = (void *) msg;
    publishData.payloadlen = msg_len;

    ret = MQTTPublish(c, topic, &publishData);

    if (MQTT_SUCCESS == ret) {
        err = kNoErr;
    } else if (MQTT_SOCKET_ERR == ret) {
        err = kConnectionErr;
    } else {
        err = kUnknownErr;
    }

    exit:
    return err;
}

void registerMqttEvents(void) {
    if (!timer_created) return;

    if (timer_status != 0) {
        mico_stop_timer(&timer_handle);
    }
    timer_status = 0;
    mico_start_timer(&timer_handle);
}

static void MqttTimerEnsureCreated(void)
{
    if (!timer_created) {
        mico_init_timer(&timer_handle, 150, UserMqttTimerFunc, NULL);
        timer_created = true;
    }
}

/* 定时器与发送队列一样常驻，只 stop 不 deinit，避免与 registerMqttEvents 竞争 */
static void MqttTimerStop(void)
{
    timer_status = 100;
    if (timer_created) mico_stop_timer(&timer_handle);
}

static void MqttClientThread(mico_thread_arg_t arg) {
    OSStatus err = kUnknownErr;

    int rc = -1;
    fd_set readfds;
    struct timeval t;
    int max_fd;

    ssl_opts ssl_settings;
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

    p_mqtt_send_msg_t p_send_msg = NULL;
    int msg_send_event_fd = -1;
    bool no_mqtt_msg_exchange = true;
    bool tcp_established = false;
    LinkStatusTypeDef link_status;

    mqtt_log("MQTT client thread started...");

    memset(&c, 0, sizeof(c));
    memset(&n, 0, sizeof(n));

    /* create msg send queue event fd */
    msg_send_event_fd = mico_create_event_fd(mqtt_msg_send_queue);

    require_action(msg_send_event_fd >= 0, exit,
                   mqtt_log("ERROR: create msg send queue event fd failed!!!"));

    MqttTimerEnsureCreated();

    /* Create a worker thread for user handling MQTT data event */
    err = mico_rtos_create_worker_thread(&mqtt_client_worker_thread, MICO_APPLICATION_PRIORITY,
                                         0x800, 5);
    require_noerr_string(err, exit, "ERROR: Unable to start the mqtt client worker thread.");
    mqtt_worker_running = true;

    mqtt_thread_should_exit = false;

    MQTT_start:

    tcp_established = false;
    isconnect = false;
    /* 1. create network connection */
    ssl_settings.ssl_enable = false;
    while (!mqtt_thread_should_exit) {
        isconnect = false;
        mico_rtos_thread_sleep(3);
        if (MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)
            continue;  //鏈厤缃甿qtt鏈嶅姟鍣ㄦ椂涓嶈繛鎺�

        micoWlanGetLinkStatus(&link_status);
        if (link_status.is_connected != 1) { mqtt_log(
                    "ERROR:WIFI not connect, waiting 3s for connecting and then connecting MQTT ");
            mico_rtos_thread_sleep(3);
            continue;
        }

        rc = NewNetwork(&n, MQTT_SERVER, MQTT_SERVER_PORT, ssl_settings);
        if (rc == MQTT_SUCCESS) { tcp_established = true; break; }

        //mqtt_log("ERROR: MQTT network connect err=%d, reconnect after 3s...", rc);
    }

    /* 收到退出请求时不能继续往下走：此时 n 里可能根本没有有效的 socket */
    if (!tcp_established) goto exit;

    mqtt_log("MQTT network connect success!");

    /* 2. init mqtt client */
    //c.heartbeat_retry_max = 2;
    rc = MQTTClientInit(&c, &n, MQTT_CMD_TIMEOUT);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client init err.");

    mqtt_log("MQTT client init success!");

    /* 3. create mqtt client connection */
    connectData.MQTTVersion = 4;  // 3: 3.1, 4: v3.1.1
    connectData.clientID.cstring = str_mac;
    connectData.username.cstring = user_config->mqtt_user;
    connectData.password.cstring = user_config->mqtt_password;
    connectData.keepAliveInterval = MQTT_CLIENT_KEEPALIVE;
    connectData.cleansession = 1;

    /* 3. create mqtt client connection */
    connectData.MQTTVersion = 4;  // 3: 3.1, 4: v3.1.1
    connectData.clientID.cstring = str_mac;
    connectData.username.cstring = user_config->mqtt_user;
    connectData.password.cstring = user_config->mqtt_password;
    connectData.keepAliveInterval = MQTT_CLIENT_KEEPALIVE;
    connectData.cleansession = 1;

    /* availability/LWT 功能在此版本回退未启用（见 v2.6.0） */
    connectData.willFlag = 0;

    rc = MQTTConnect(&c, &connectData);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client connect err.");

    mqtt_log("MQTT client connect success, result: %d ", rc);

    UserLedSet(RelayOut() && user_config->power_led_enabled);

    /* 4. mqtt client subscribe */
    rc = MQTTSubscribe(&c, topic_set, QOS0, MessageArrived);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client subscribe err.");mqtt_log(
            "MQTT client subscribe success! recv_topic=[%s].", topic_set);
    /*4.1 连接成功后先更新一次数据*/
    isconnect = true;

    int i = 0;
    for (; i < SOCKET_NUM; i++) {
        UserMqttSendSocketState(i);
    }

    UserMqttSendLedState();
    UserMqttSendTotalSocketState();
    UserMqttSendChildLockState();

    registerMqttEvents();
    /* 5. client loop for recv msg && keepalive */
    while (!mqtt_thread_should_exit) {
        isconnect = true;
        no_mqtt_msg_exchange = true;

        /* WiFi 断开后 socket 已失效，fd 号还可能被其它连接复用，
         * 必须立刻拆链，而不是继续在死连接上 select/收发 */
        micoWlanGetLinkStatus(&link_status);
        if (link_status.is_connected != 1) {
            mqtt_log("wifi link down, tear down mqtt connection");
            err = kConnectionErr;
            rc = -1;
            goto MQTT_reconnect;
        }

        if (c.ipstack == NULL || c.ipstack->my_socket < 0) {
            mqtt_log("mqtt socket invalid");
            err = kConnectionErr;
            rc = -1;
            goto MQTT_reconnect;
        }

        FD_ZERO(&readfds);
        FD_SET(c.ipstack->my_socket, &readfds);
        FD_SET(msg_send_event_fd, &readfds);
        /* nfds 必须是最大fd+1。原来固定用 msg_send_event_fd+1，
         * 当 mqtt socket 的 fd 更大时 select 根本不会监视它 -> 收不到任何下行消息 */
        max_fd = (c.ipstack->my_socket > msg_send_event_fd)
                 ? c.ipstack->my_socket : msg_send_event_fd;
        /* 每轮重新初始化：select 允许改写 timeval，复用同一变量会让超时越来越短 */
        t.tv_sec = MQTT_POLL_TMIE / 1000;
        t.tv_usec = (MQTT_POLL_TMIE % 1000) * 1000;

        rc = select(max_fd + 1, &readfds, NULL, NULL, &t);
        if (rc < 0) {
            if (mqtt_thread_should_exit) break;
            mqtt_log("select error, will reconnect");
            err = kConnectionErr;
            goto MQTT_reconnect;
        }

        if (rc > 0) {
            /* recv msg from server */
            if (FD_ISSET(c.ipstack->my_socket, &readfds)) {
                rc = MQTTYield(&c, (int) MQTT_YIELD_TMIE);
                require_noerr(rc, MQTT_reconnect);
                no_mqtt_msg_exchange = false;
            }

            /* recv msg from user worker thread to be sent to server */
            if (FD_ISSET(msg_send_event_fd, &readfds)) {
                while (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) == false) {
                    // get msg from send queue
                    mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
                    if (p_send_msg == NULL) {
                        mqtt_log("ERROR: null entry in send queue");
                        break;
                    }

                    // send message to server
                    err = MqttMsgPublish(&c, p_send_msg->topic, p_send_msg->qos,
                                         p_send_msg->retained,
                                         (const unsigned char *) p_send_msg->data,
                                         p_send_msg->datalen);
                    free(p_send_msg);
                    p_send_msg = NULL;
                    require_noerr_string(err, MQTT_reconnect, "ERROR: MQTT publish data err");

                    //mqtt_log("MQTT publish data success! send_topic=[%s], msg=[%ld].", p_send_msg->topic, p_send_msg->datalen);
                    no_mqtt_msg_exchange = false;
                }
            }
        }

        /* if no msg exchange, we need to check ping msg to keep alive. */
        if (no_mqtt_msg_exchange) {
            rc = keepalive(&c);
            require_noerr_string(rc, MQTT_reconnect, "ERROR: keepalive err");
        }
    }

    /* 循环自然结束 == 收到了退出请求 */
    err = kNoErr;
    rc = MQTT_SUCCESS;

    MQTT_reconnect:

mqtt_log("Disconnect MQTT client, reason: mqtt_rc = %d, err = %d", rc, err);

    MqttTimerStop();
    clear_mqtt_msg_send_queue();
    UserMqttClientRelease(&c, &n);
    isconnect = false;
    UserLedSet(-1);
    mico_rtos_thread_msleep(100);
    UserLedSet(-1);

    if (mqtt_thread_should_exit) goto exit;

    mico_rtos_thread_sleep(5);
    goto MQTT_start;

exit:
    isconnect = false;
    mqtt_log("EXIT: MQTT client exit with err = %d.", err);

    MqttTimerStop();
    clear_mqtt_msg_send_queue();
    UserMqttClientRelease(&c, &n);

    if (mqtt_worker_running) {
        mico_rtos_delete_worker_thread(&mqtt_client_worker_thread);
        mqtt_worker_running = false;
    }
    if (msg_send_event_fd >= 0) {
        mico_delete_event_fd(msg_send_event_fd);
        msg_send_event_fd = -1;
    }

    /* 复位状态，让 UserMqttInit 之后可以重新拉起本线程 */
    MqttLifecycleLock();
    mqtt_thread_running = false;
    mqtt_thread_should_exit = false;
    MqttLifecycleUnlock();

    mqtt_log("mqtt client thread stopped.");
    mico_rtos_delete_thread(NULL); // 自删
    return;
}

// callback, msg received from mqtt server
static void MessageArrived(MessageData *md) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = NULL;
    MQTTMessage *message = md->message;
    unsigned int topic_len, data_len;

    if (md == NULL || message == NULL || message->payload == NULL) return;

    p_recv_msg = (p_mqtt_recv_msg_t) calloc(1, sizeof(mqtt_recv_msg_t));
    require_action(p_recv_msg, exit, err = kNoMemoryErr);

    /* 长度必须裁剪：payloadlen/topic 长度都来自对端，原来直接 memcpy/strncpy
     * 会把固定大小的结构体冲穿（堆溢出） */
    topic_len = (md->topicName && md->topicName->lenstring.data)
                ? (unsigned int) md->topicName->lenstring.len : 0;
    if (topic_len > MAX_MQTT_TOPIC_SIZE - 1) topic_len = MAX_MQTT_TOPIC_SIZE - 1;

    data_len = (unsigned int) message->payloadlen;
    if (data_len > MAX_MQTT_DATA_SIZE - 1) {
        mqtt_log("WARN: oversized mqtt payload (%u), truncated", data_len);
        data_len = MAX_MQTT_DATA_SIZE - 1;
    }

    memcpy(p_recv_msg->topic, md->topicName->lenstring.data, topic_len);
    p_recv_msg->topic[topic_len] = 0;
    memcpy(p_recv_msg->data, message->payload, data_len);
    p_recv_msg->data[data_len] = 0;   /* calloc 已清零，这里显式保证字符串安全 */

    p_recv_msg->datalen = data_len;
    p_recv_msg->qos = (char) (message->qos);
    p_recv_msg->retained = message->retained;

    mqtt_log("MessageArrived topic[%s] data[%.*s]", p_recv_msg->topic, (int) data_len,
             p_recv_msg->data);
    err = mico_rtos_send_asynchronous_event(&mqtt_client_worker_thread, UserRecvHandler,
                                            p_recv_msg);
    require_noerr(err, exit);

    exit:
    if (err != kNoErr) { mqtt_log("ERROR: Recv data err = %d", err);
        if (p_recv_msg) free(p_recv_msg);
    }
    return;
}

/* Application process MQTT received data */
OSStatus UserRecvHandler(void *arg) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = arg;
    require(p_recv_msg, exit);

    mqtt_log("user get data success! from_topic=[%s], msg=[%ld].", p_recv_msg->topic,
             p_recv_msg->datalen);
    //UserFunctionCmdReceived(0, p_recv_msg->data);

    ProcessHaCmd(p_recv_msg->data);

    free(p_recv_msg);

    exit:
    return err;
}

void ProcessHaCmd(char *cmd) {
    mqtt_log("ProcessHaCmd[%s]", cmd);
    char mac[20] = {0};

    if (cmd == NULL) return;

    if (strcmp(cmd, "set socket") == ' ') {
        int i = -1, on = -1;
        /* %19s 限定宽度：mac 只有 20 字节，不限长会直接冲穿栈 */
        if (sscanf(cmd, "set socket %19s %d %d", mac, &i, &on) != 3) return;
        if (strcmp(mac, str_mac)) return;
        if (i < 0 || i >= SOCKET_NUM || (on != 0 && on != 1)) {
            mqtt_log("set socket args out of range: %d %d", i, on);
            return;
        }
        mqtt_log("set socket[%d] on[%d]", i, on);
        UserRelaySet(i, on);
        UserMqttSendSocketState(i);
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
    } else if (strcmp(cmd, "set led") == ' ') {
        int on = -1;
        if (sscanf(cmd, "set led %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;
        if (on != 0 && on != 1) return;
        mqtt_log("set led on[%d]", on);
        user_config->power_led_enabled = on;
        if (RelayOut() && user_config->power_led_enabled) {
            UserLedSet(1);
        } else {
            UserLedSet(0);
        }
        UserMqttSendLedState();
        mico_system_context_update(sys_config);
    } else if (strcmp(cmd, "set total_socket") == ' ') {
        int on = -1;
        if (sscanf(cmd, "set total_socket %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;
        if (on != 0 && on != 1) return;
        mqtt_log("set total_socket on[%d]", on);
        UserRelaySetAll(on);
        int i = 0;
        for (i = 0; i < SOCKET_NUM; i++) {
            UserMqttSendSocketState(i);
        }
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
    }else if (strcmp(cmd, "set childLock") == ' ') {
        int on = -1;
        if (sscanf(cmd, "set childLock %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;
        if (on != 0 && on != 1) return;
        mqtt_log("set childLock on[%d]", on);
        user_config->user[0] = on;
        childLockEnabled = on;
        UserMqttSendChildLockState();
        mico_system_context_update(sys_config);
    }else if (strcmp(cmd, "reboot") == ' ') {
        if (sscanf(cmd, "reboot %19s", mac) != 1) return;
        if (strcmp(mac, str_mac)) return;
        mqtt_log("reboot requested");
        /* 先把配置落盘，避免重启后丢掉未保存的改动 */
        mico_system_context_update(sys_config);
        MicoSystemReboot();  // 立即重启设备
    }
}

OSStatus UserMqttSendTopic(char *topic, char *arg, char retained) {
    return UserMqttSendTopicQos(topic, arg, retained, 0);
}

/*
 * qos=1 时状态类发布交给 broker 重传：总开关一次会突发多条 QoS0，
 * WiFi 拥塞时丢 1~2 条，那条 retained 就永远错下去（前端和实际继电器不一致
 * 且无人自愈）。QoS1 + 周期性全量重发是两端都能自愈的最小代价方案。
 */
OSStatus UserMqttSendTopicQos(char *topic, char *arg, char retained, char qos) {
    OSStatus err = kUnknownErr;
    p_mqtt_send_msg_t p_send_msg = NULL;
    size_t datalen;

    if (mqtt_msg_send_queue == NULL || !isconnect || topic == NULL || arg == NULL) {
        return err;
    }

    if (qos != 0 && qos != 1) qos = 0;   /* 协议栈只实现了 QOS0/QOS1 */

//  mqtt_log("======App prepare to send ![%d]======", MicoGetMemoryInfo()->free_memory);

    /* Send queue is full, pop the oldest */
    if (mico_rtos_is_queue_full(&mqtt_msg_send_queue) == true) {
        mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
        if (p_send_msg) free(p_send_msg);
        p_send_msg = NULL;
    }

    /* Push the latest data into send queue*/
    p_send_msg = (p_mqtt_send_msg_t) calloc(1, sizeof(mqtt_send_msg_t));
    require_action(p_send_msg, exit, err = kNoMemoryErr);

    /* 长度必须裁剪到结构体容量，否则就是堆溢出 */
    datalen = strlen(arg);
    if (datalen > MAX_MQTT_DATA_SIZE - 1) {
        mqtt_log("WARN: oversized publish (%d), truncated", (int) datalen);
        datalen = MAX_MQTT_DATA_SIZE - 1;
    }

    p_send_msg->qos = qos;
    p_send_msg->retained = retained;
    p_send_msg->datalen = (uint32_t) datalen;
    memcpy(p_send_msg->data, arg, datalen);
    p_send_msg->data[datalen] = 0;
    strncpy(p_send_msg->topic, topic, MAX_MQTT_TOPIC_SIZE - 1);
    p_send_msg->topic[MAX_MQTT_TOPIC_SIZE - 1] = 0;

    err = mico_rtos_push_to_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
    require_noerr(err, exit);

    //mqtt_log("Push user msg into send queue success!");

    exit:
    if (err != kNoErr && p_send_msg) free(p_send_msg);
    return err;
}

/* Application collect data and seng them to MQTT send queue */
OSStatus UserMqttSend(char *arg) {
    return UserMqttSendTopic(topic_state, arg, 0);
}

/*
 * 上线/下线宣告。retained + QoS1：离线通知是一定要送达的，
 * 且新订阅者连上来时要立刻知道当前是否在线。
 */
void UserMqttSendAvailability(char online) {
    char *payload = (char *) (online ? MQTT_CLIENT_AVAIL_ON : MQTT_CLIENT_AVAIL_OFF);

    if (topic_availability[0] == 0) {
        /* 线程外调用（极端时序）时兜底拼一次 topic */
        snprintf(topic_availability, MAX_MQTT_TOPIC_SIZE, MQTT_CLIENT_AVAIL_TOPIC, str_mac);
    }
    UserMqttSendTopicQos(topic_availability, (char *) payload, 1, 1);
}

//鏇存柊ha寮�鍏崇姸鎬�
OSStatus UserMqttSendSocketState(char socket_id) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/socket_%d/state", str_mac, (int) socket_id);
        snprintf(send_buf, 64, "set socket %s %d %d", str_mac, socket_id,
                (int) user_config->socket_status[(int) socket_id]);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendTotalSocketState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/total_socket/state", str_mac);
        snprintf(send_buf, 64, "set total_socket %s %d", str_mac, RelayOut() ? 1 : 0);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendLedState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/led/state", str_mac);
        snprintf(send_buf, 64, "set led %s %d", str_mac, (int) user_config->power_led_enabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendChildLockState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/childLock/state", str_mac);
        snprintf(send_buf, 64, "set childLock %s %d", str_mac, childLockEnabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

//hass mqtt鑷姩鍙戠幇鏁版嵁寮�鍏冲彂閫�
void UserMqttHassAuto(char socket_id) {
    char *send_buf = NULL;
    char *topic_buf = NULL;

    socket_id--;
    if (socket_id < 0 || socket_id >= SOCKET_NUM) {
        mqtt_log("WARN: UserMqttHassAuto bad socket index %d", socket_id);
        return;
    }

    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/socket_%d/config", str_mac, socket_id);
        snprintf(send_buf, 600,
                "{\"name\":\"%s\","
                "\"uniq_id\":\"tc1_%s_s%d\","
                "\"object_id\":\"tc1_%s_s%d\","
                "\"stat_t\":\"homeassistant/switch/%s/socket_%d/state\","
                "\"cmd_t\":\"device/ztc1/set\","
                "\"pl_on\":\"set socket %s %d 1\","
                "\"pl_off\":\"set socket %s %d 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                user_config->socket_names[(int)socket_id], str_mac, socket_id,str_mac, socket_id, str_mac, socket_id,
                str_mac,
                socket_id, str_mac, socket_id, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoRebootButton(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        // 重启按钮配置
        snprintf(topic_buf, 64, "homeassistant/button/%s/reboot/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"重启设备\","
                "\"uniq_id\":\"tc1_%s_reboot\","
                "\"object_id\":\"tc1_%s_reboot\","
                "\"cmd_t\":\"device/ztc1/set\","
                "\"pl_prs\":\"reboot %s\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac,str_mac, sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

void UserMqttHassAutoLed(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/led/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"LED指示灯\","
                "\"uniq_id\":\"tc1_%s_led\","
                "\"object_id\":\"tc1_%s_led\","
                "\"stat_t\":\"homeassistant/switch/%s/led/state\","
                "\"cmd_t\":\"device/ztc1/set\","
                "\"pl_on\":\"set led %s 1\","
                "\"pl_off\":\"set led %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoChildLock(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/childLock/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"童锁\","
                "\"uniq_id\":\"tc1_%s_child_lock\","
                "\"object_id\":\"tc1_%s_child_lock\","
                "\"stat_t\":\"homeassistant/switch/%s/childLock/state\","
                "\"cmd_t\":\"device/ztc1/set\","
                "\"pl_on\":\"set childLock %s 1\","
                "\"pl_off\":\"set childLock %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoTotalSocket(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 64, "homeassistant/switch/%s/total_socket/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"总开关\","
                "\"uniq_id\":\"tc1_%s_total_socket\","
                "\"object_id\":\"tc1_%s_total_socket\","
                "\"stat_t\":\"homeassistant/switch/%s/total_socket/state\","
                "\"cmd_t\":\"device/ztc1/set\","
                "\"pl_on\":\"set total_socket %s 1\","
                "\"pl_off\":\"set total_socket %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

//hass mqtt鑷姩鍙戠幇鏁版嵁鍔熺巼鍙戦��
void UserMqttHassAutoPower(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = malloc(600);
    topic_buf = malloc(128);
    if (send_buf != NULL && topic_buf != NULL) {
        snprintf(topic_buf, 128, "homeassistant/sensor/%s/power/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"功率\","
                "\"uniq_id\":\"tc1_%s_p\","
                "\"object_id\":\"tc1_%s_p\","
                "\"state_topic\":\"homeassistant/sensor/%s/power/state\","
                "\"unit_of_measurement\":\"W\","
                "\"icon\":\"mdi:gauge\","
                "\"value_template\":\"{{ value_json.power }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
        snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumption/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"总耗电量\","
                "\"uniq_id\":\"tc1_%s_pc\","
                "\"object_id\":\"tc1_%s_pc\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumption/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumption }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);


        snprintf(topic_buf, 128, "homeassistant/sensor/%s/startupTime/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"运行时间\","
                "\"uniq_id\":\"tc1_%s_sut\","
                "\"object_id\":\"tc1_%s_sut\","
                "\"state_topic\":\"homeassistant/sensor/%s/startupTime/state\","
                "\"icon\":\"mdi:clock-time-three-outline\","
                "\"entity_category\":\"diagnostic\","
                "\"value_template\":\"{{ value_json.startupTime }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumptionToday/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"今日耗电量\","
                "\"uniq_id\":\"tc1_%s_pc_today\","
                "\"object_id\":\"tc1_%s_pc_today\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionToday/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionToday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumptionYesterday/config", str_mac);
        snprintf(send_buf, 600,
                "{\"name\":\"昨日耗电量\","
                "\"uniq_id\":\"tc1_%s_pc_yesterday\","
                "\"object_id\":\"tc1_%s_pc_yesterday\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionYesterday/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionYesterday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

char topic_buf[128] = {0};
char send_buf[128] = {0};

/* 脉冲数 -> kWh。power_pulse_uwh 默认 475，与旧代码的 17.1/1000/36000 完全等价 */
static double pulses_to_kwh(long pulses) {
    if (pulses < 0) pulses = 0;
    return ((double) pulses * (double) power_pulse_uwh) / 1000000000.0;
}


/*
 * 全量状态重发：任意端自愈的根本手段。
 *
 * QoS1 解决"发布丢失"，但 retained 的错误值不会因为补发一次就修好——
 * 好在命令幂等（set socket mac idx on 本身就是全量真值），周期性把
 * 6 路状态 + 总开关/LED/童锁 重发一遍，任何错过回显的订阅端（浏览器、HA、
 * 后端采集器）都会被纠正。
 *
 * 注意分批：发送队列只有 MAX_MQTT_SEND_QUEUE_SIZE(10) 项，一次全量
 * 正好打满（6+1+1+1+1），会与用户操作/功率上报抢占队列。分批夹 sleep
 * 让 MQTT 线程有机会消化，避免把最紧急的用户回显挤出队列。
 */
void UserMqttRepublishAllStates(void) {
    int i;

    /*
     * 队列水位保护：重发一次恰好是 10 条（=队列深度），如果用户此刻
     * 也在下命令，UserMqttSendTopic 的队满策略是"丢最旧"，会把用户的
     * 命令挤掉。宁可本周期跳过（下个 30s 周期自然会补），也不抢用户带宽。
     */
    if (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) != true) {
        mqtt_log("republish deferred: send queue busy");
        return;
    }

    for (i = 0; i < SOCKET_NUM; i++) {
        UserMqttSendSocketState(i);
        mico_rtos_thread_msleep(60);
    }
    UserMqttSendTotalSocketState();
    UserMqttSendLedState();
    UserMqttSendChildLockState();
    /* UserMqttSendAvailability(1); */
}

extern void UserMqttHassPower(void) {
    long today = (long) p_count - (long) user_config->p_count_1_day_ago;
    long yesterday = (long) user_config->p_count_1_day_ago
                     - (long) user_config->p_count_2_days_ago;

    snprintf(topic_buf, 128, "homeassistant/sensor/%s/power/state", str_mac);
    /* real_time_power 单位是 0.1W */
    snprintf(send_buf, 600, "{\"power\":\"%.3f\"}", (double) real_time_power / 10.0);
    UserMqttSendTopic(topic_buf, send_buf, 0);

    snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumption/state", str_mac);
    snprintf(send_buf, 600, "{\"powerConsumption\":\"%.3f\"}", pulses_to_kwh((long) p_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    //计算系统运行时间
    char up_time[24] = "00:00:00";
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    int past = past_ms / 1000;
    int d = past / 3600 / 24;
    int h = past / 3600 % 24;
    int m = past / 60 % 60;
    int s = past % 60;
    snprintf(up_time, sizeof(up_time), "%d - %02d:%02d:%02d", d, h, m, s);

    snprintf(topic_buf, 128, "homeassistant/sensor/%s/startupTime/state", str_mac);
    snprintf(send_buf, 600, "{\"startupTime\":\"%s\"}", up_time);
    UserMqttSendTopic(topic_buf, send_buf, 0);

    snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumptionToday/state", str_mac);
    snprintf(send_buf, 600, "{\"powerConsumptionToday\":\"%.3f\"}", pulses_to_kwh(today));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    snprintf(topic_buf, 128, "homeassistant/sensor/%s/powerConsumptionYesterday/state", str_mac);
    snprintf(send_buf, 600, "{\"powerConsumptionYesterday\":\"%.3f\"}", pulses_to_kwh(yesterday));
    UserMqttSendTopic(topic_buf, send_buf, 0);
}

bool UserMqttIsConnect() {
    return isconnect;
}
