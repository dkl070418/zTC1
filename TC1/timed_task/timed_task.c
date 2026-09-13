#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<stdbool.h>
#include<time.h>

#include"main.h"
#include"user_gpio.h"
#include "mqtt_server/user_mqtt_client.h"
#include"timed_task/timed_task.h"
#include"http_server/web_log.h"
#include "user_wifi.h"

int day_sec = 86400;

/* 任务链表同时被主线程 ProcessTask 与 HTTP 线程读写，必须串行化。
 * 互斥量非递归：所有 _nolock 内部函数只在已持锁时调用。 */
static mico_mutex_t task_mutex;
static bool task_mutex_ready = false;

void TaskSubsysInit(void)
{
    if (task_mutex_ready) return;
    if (mico_rtos_init_mutex(&task_mutex) == kNoErr) {
        task_mutex_ready = true;
    } else {
        task_log("ERROR: task mutex init fail");
    }
}

static void TaskLock(void)
{
    if (task_mutex_ready) mico_rtos_lock_mutex(&task_mutex);
}

static void TaskUnlock(void)
{
    if (task_mutex_ready) mico_rtos_unlock_mutex(&task_mutex);
}

static int TaskIndexOf(pTimedTask t)
{
    return (int) (t - &user_config->timed_tasks[0]);
}

static pTimedTask NewTask_nolock(void)
{
    for (int i = 0; i < MAX_TASK_NUM; i++)
    {
        pTimedTask task = &user_config->timed_tasks[i];
        if (!task->on_use)
        {
            task->on_use = true;
            task->next = NULL;
            return task;
        }
    }
    return NULL;
}

pTimedTask NewTask()
{
    pTimedTask t;
    TaskLock();
    t = NewTask_nolock();
    TaskUnlock();
    return t;
}

static bool AddTaskSingle_nolock(pTimedTask task)
{
    user_config->task_count++;
    if (user_config->task_top == NULL)
    {
        task->next = NULL;
        user_config->task_top = task;
        return true;
    }

    if (task->prs_time <= user_config->task_top->prs_time)
    {
        task->next = user_config->task_top;
        user_config->task_top = task;
        return true;
    }

    pTimedTask tmp = user_config->task_top;
    while (tmp)
    {
        if (tmp->next == NULL
            || (task->prs_time >= tmp->prs_time
             && task->prs_time < tmp->next->prs_time))
        {
            task->next = tmp->next;
            tmp->next = task;
            return true;
        }
        tmp = tmp->next;
    }
    user_config->task_count--;
    return false;
}

/* day: 1=周一 ... 7=周日；weekday 模式 9=工作日 10=周末 */
static bool WeekdayModeMatch(int mode, int day)
{
    if (mode == 9) return day >= 1 && day <= 5;
    if (mode == 10) return day >= 6 && day <= 7;
    return false;
}

static bool AddTaskWeek_nolock(pTimedTask task)
{
    time_t now = time(NULL);
    int today_weekday = (now / day_sec + 3) % 7 + 1; //1970-01-01 星期五
    int tod = (int) (now % day_sec);
    int hit_sec = (int) (task->prs_time % day_sec);
    int offset;

    if (task->weekday == 9 || task->weekday == 10) {
        for (offset = 0; offset < 7; offset++) {
            int day = ((today_weekday - 1 + offset) % 7) + 1;
            if (!WeekdayModeMatch(task->weekday, day)) continue;
            if (offset == 0 && hit_sec <= tod) continue;
            task->prs_time = (now - now % day_sec) + (offset * day_sec) + hit_sec;
            return AddTaskSingle_nolock(task);
        }
        return false;
    }

    {
        int next_day = task->weekday - today_weekday;
        bool next_day_is_today = next_day == 0 && hit_sec > tod;
        next_day = next_day > 0 || next_day_is_today ? next_day : next_day + 7;
        task->prs_time = (now - now % day_sec) + (next_day * day_sec) + hit_sec;
    }

    return AddTaskSingle_nolock(task);
}

static bool AddTask_nolock(pTimedTask task)
{
    if (task->weekday == 0 || task->weekday == 8)
        return AddTaskSingle_nolock(task);
    return AddTaskWeek_nolock(task);
}

bool AddTask(pTimedTask task)
{
    bool ok;
    TaskLock();
    ok = AddTask_nolock(task);
    if (ok) mico_system_context_update(sys_config);
    TaskUnlock();
    return ok;
}

static bool DelFirstTask_nolock(void)
{
    if (user_config->task_top)
    {
        pTimedTask tmp = user_config->task_top;
        user_config->task_top = user_config->task_top->next;
        user_config->task_count--;
        if (tmp->weekday == 0)
        {
            tmp->on_use = false;
            tmp->next = NULL;
        }
        else if (tmp->weekday == 8) //8代表每日任务
        {
            tmp->prs_time += day_sec;
            AddTask_nolock(tmp);
        }
        else if (tmp->weekday == 9 || tmp->weekday == 10)
        {
            tmp->prs_time += day_sec;
            AddTask_nolock(tmp);
        }
        else
        {
            tmp->prs_time += 7 * day_sec;
            AddTask_nolock(tmp);
        }
        return true;
    }
    return false;
}

bool DelFirstTask()
{
    bool ok;
    TaskLock();
    ok = DelFirstTask_nolock();
    TaskUnlock();
    return ok;
}

bool DelTaskById(int id)
{
    pTimedTask target, pre;

    if (id < 0 || id >= MAX_TASK_NUM) return false;

    TaskLock();
    target = &user_config->timed_tasks[id];
    if (!target->on_use) {
        TaskUnlock();
        return false;
    }

    if (user_config->task_top == target) {
        user_config->task_top = target->next;
    } else {
        pre = user_config->task_top;
        while (pre && pre->next != target) pre = pre->next;
        if (pre == NULL) {
            /* 槽位标了 on_use 但不在链表：当残留清理 */
            target->on_use = false;
            target->next = NULL;
            TaskUnlock();
            return false;
        }
        pre->next = target->next;
    }
    target->on_use = false;
    target->next = NULL;
    if (user_config->task_count > 0) user_config->task_count--;
    mico_system_context_update(sys_config);
    task_log("DelTaskById id=%d left=%d", id, user_config->task_count);
    TaskUnlock();
    return true;
}

bool DelTask(int time)
{
    /* 兼容旧接口：按时间戳删（可能撞车，新前端请用 DelTaskById） */
    int i;
    bool ok = false;

    TaskLock();
    for (i = 0; i < MAX_TASK_NUM; i++) {
        pTimedTask t = &user_config->timed_tasks[i];
        if (!t->on_use) continue;
        if ((int) t->prs_time != time) continue;

        if (user_config->task_top == t) {
            user_config->task_top = t->next;
        } else {
            pTimedTask pre = user_config->task_top;
            while (pre && pre->next != t) pre = pre->next;
            if (pre) pre->next = t->next;
        }
        t->on_use = false;
        t->next = NULL;
        if (user_config->task_count > 0) user_config->task_count--;
        ok = true;
        break;
    }
    if (ok) mico_system_context_update(sys_config);
    TaskUnlock();
    return ok;
}

void ProcessTask()
{
    TaskLock();
    if (user_config->task_top == NULL) {
        TaskUnlock();
        return;
    }
    task_log("process task time[%ld] operation[%s] on[%d]",
        user_config->task_top->prs_time, get_func_name(user_config->task_top->operation), user_config->task_top->on);
    switch (user_config->task_top->operation) {
            case SWITCH_ALL_SOCKETS:
                UserRelaySetAll(user_config->task_top->on);
                mico_system_context_update(sys_config);
                for (int i = 0; i < SOCKET_NUM; i++) {
                    UserMqttSendSocketState(i);
                }
                UserMqttSendTotalSocketState();
                break;
            case SWITCH_SOCKET_1:
            case SWITCH_SOCKET_2:
            case SWITCH_SOCKET_3:
            case SWITCH_SOCKET_4:
            case SWITCH_SOCKET_5:
            case SWITCH_SOCKET_6:
                UserRelaySet(user_config->task_top->operation - 1, user_config->task_top->on);
                UserMqttSendSocketState(user_config->task_top->operation - 1);
                UserMqttSendTotalSocketState();
                mico_system_context_update(sys_config);
                break;
            case SWITCH_LED_ENABLE:
                if (RelayOut() && user_config->task_top->on) {
                    UserLedSet(1);
                } else {
                    UserLedSet(0);
                }
                UserMqttSendLedState();
                mico_system_context_update(sys_config);
                break;
            case SWITCH_CHILD_LOCK_ENABLE:
                user_config->user[0] = user_config->task_top->on;
                childLockEnabled = user_config->user[0];
                mico_system_context_update(sys_config);
                UserMqttSendChildLockState();
                break;
            case REBOOT_SYSTEM:
                MicoSystemReboot();
                break;
            case CONFIG_WIFI:
                micoWlanSuspendStation();
                ApInit(true);
                break;
            case RESET_SYSTEM:
                mico_system_context_restore(sys_config);
                mico_rtos_thread_sleep(1);
                MicoSystemReboot();
                break;
            default:
                break;
        }
    DelFirstTask_nolock();
    mico_system_context_update(sys_config);
    TaskUnlock();
}

char* GetTaskStr()
{
    int count;
    size_t alloc;
    char* str;
    pTimedTask tmp_tsk;
    char* tmp_str;

    TaskLock();
    count = user_config->task_count;
    if (count < 0) count = 0;
    if (count > MAX_TASK_NUM) count = MAX_TASK_NUM;

    /* 每条约 100B + id 字段；按上限 128 分配并判空 */
    alloc = (size_t) count * 140 + 4;
    if (alloc < 16) alloc = 16;
    str = (char*) malloc(alloc);
    if (str == NULL) {
        TaskUnlock();
        str = (char*) malloc(4);
        if (str) { str[0] = '['; str[1] = ']'; str[2] = 0; }
        return str;
    }

    tmp_tsk = user_config->task_top;
    tmp_str = str;
    tmp_str[0] = '[';
    tmp_str[1] = 0;
    tmp_str++;

    while (tmp_tsk)
    {
        char buffer[26];
        struct tm tm_info;
        time_t prs_time = tmp_tsk->prs_time + 28800;
        int n;
        int id = TaskIndexOf(tmp_tsk);

        if (localtime_r(&prs_time, &tm_info) == NULL)
            memset(&tm_info, 0, sizeof(tm_info));
        strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &tm_info);

        n = snprintf(tmp_str, (size_t) (alloc - (size_t) (tmp_str - str)),
            "{'id':%d,'timestamp':%ld,'prs_time':'%s','operation':%d,'on':%d,'weekday':%d},",
            id, (long) tmp_tsk->prs_time, buffer, tmp_tsk->operation, tmp_tsk->on, tmp_tsk->weekday);
        if (n < 0 || (size_t) n >= (size_t) (alloc - (size_t) (tmp_str - str))) break;
        tmp_str += n;
        tmp_tsk = tmp_tsk->next;
    }
    if (tmp_str > str + 1) --tmp_str;
    *tmp_str = ']';
    *(tmp_str + 1) = 0;
    TaskUnlock();
    return str;
}
