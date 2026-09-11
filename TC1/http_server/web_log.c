#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mico.h"


#include"http_server/web_log.h"

LogRecord log_record = { 0,{ 0 } };
/* 拼接缓冲：头部行号 + LOG_NUM*(127+1) + 最后一行 FreeMem 附加，留足余量 */
char log_record_str[LOG_NUM*LOG_LEN + 64] = { 0 };

static mico_mutex_t log_mutex;
static bool log_mutex_ready = false;

void WebLogInit(void)
{
    if (!log_mutex_ready) {
        if (mico_rtos_init_mutex(&log_mutex) == kNoErr)
            log_mutex_ready = true;
    }
}

static void LogLock(void)
{
    if (log_mutex_ready) mico_rtos_lock_mutex(&log_mutex);
}

static void LogUnlock(void)
{
    if (log_mutex_ready) mico_rtos_unlock_mutex(&log_mutex);
}

/*
 * 把 log 放进环形缓冲区。log 必须是 LOG_LEN 字节大小的缓冲区，
 * 所有权随之转移（由本模块负责 free）。
 */
void SetLogRecord(LogRecord* lr, char* log)
{
    char** p_log;

    if (lr == NULL || log == NULL) return;

    /* 原实现用 strlen(log) > LOG_LEN 判断，若 log 未结尾会读越界；
     * 这里直接强制在容量边界处截断。 */
    log[LOG_LEN - 1] = 0;

    LogLock();
    p_log = &lr->logs[++lr->idx % LOG_NUM];
    if (*p_log)
    {
        free(*p_log);
    }
    *p_log = log;
    LogUnlock();
}

/* 往 *dst 追加一段格式化文本，并推进剩余空间；空间耗尽时保持NUL结尾 */
static void LogAppendF(char **dst, size_t *left, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*left == 0) return;

    va_start(ap, fmt);
    n = vsnprintf(*dst, *left, fmt, ap);
    va_end(ap);

    if (n < 0) return;

    if ((size_t) n >= *left) {
        /* 被截断：vsnprintf 已写入 *left-1 个字符 + NUL */
        *dst += *left - 1;
        *left = 0;
    } else {
        *dst += n;
        *left -= (size_t) n;
    }
}

/*
 * 把环形缓冲里的日志拼成一整段返回。
 *
 * 缓冲大小由 LOG_NUM 推导，与 ring 同步：最坏情况下每条 127 字符+'\n'，
 * 加上头部行号与最后一行的 FreeMem 附加信息，LOG_BUF_SIZE 有富余，
 * 且写入全部走带边界的 LogAppendF，溢出在此处不可能发生。
 */
char* GetLogRecord(void)
{
    char *tmp = log_record_str;
    size_t left = sizeof(log_record_str);
    unsigned int i, end;

    LogLock();

    end = log_record.idx;
    i = (end + 1 >= LOG_NUM) ? (end - LOG_NUM + 1) : 0;

    LogAppendF(&tmp, &left, "%u\n", end);

    for (; i <= end && left > 1; i++) {
        char *line = log_record.logs[i % LOG_NUM];
        if (!line) continue;

        if (i == end)
            LogAppendF(&tmp, &left, "%s\nFreeMem %d bytes\n", line,
                       (int) MicoGetMemoryInfo()->free_memory);
        else
            LogAppendF(&tmp, &left, "%s\n", line);
    }

    LogUnlock();

    return log_record_str;
}

void WebLogFmt(const char *fmt, ...)
{
    char *buff = (char *) malloc(sizeof(char) * LOG_LEN);
    va_list ap;
    time_t now;
    struct tm tm_now;

    if (buff == NULL) return;

    now = time(NULL) + 28800; //东8区
    if (localtime_r(&now, &tm_now) == NULL) {
        memset(&tm_now, 0, sizeof(tm_now));
    }
    /* 不能用 localtime()：它返回静态缓冲区，多线程下会被互相改写 */
    strftime(buff, TIME_LEN, "[%Y-%m-%d %H:%M:%S]", &tm_now);
    buff[TIME_LEN - 1] = ' ';

    va_start(ap, fmt);
    vsnprintf(buff + TIME_LEN, LOG_LEN - TIME_LEN, fmt, ap);
    va_end(ap);
    buff[LOG_LEN - 1] = 0;

    SetLogRecord(&log_record, buff);
}
