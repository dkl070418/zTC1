#include <time.h>

#ifndef WEB_LOG_H
#define WEB_LOG_H

/*
 * 日志环形缓冲区。
 *
 * 这两块是静态 RAM 的大头：ring + 拼接缓冲 共 LOG_NUM*LOG_LEN*2 字节。
 * 调试用即可，40 条足够回溯一次配网/开关操作（约 5KB），
 * 之前 100 条占了 25.6KB，把运行时空闲堆压到只剩十几 KB。
 * 若要加长回溯，优先考虑改成"读时逐条发"而不是继续加大缓冲。
 */
#define LOG_NUM 40
#define LOG_LEN 128
#define TIME_LEN 22

typedef struct
{
    unsigned int idx;
    char* logs[LOG_NUM];
} LogRecord;

/* log 必须是 LOG_LEN 字节、且所有权转移给日志环形缓冲区的缓冲区 */
void SetLogRecord(LogRecord* lr, char* log);
char* GetLogRecord(void);
void WebLogFmt(const char *fmt, ...);
void WebLogInit(void);

/*
 * 旧实现在宏里使用全局的 LOG_TMP/LOG_NOW 暂存 malloc 出来的缓冲区，
 * 多个线程同时打日志会互相覆盖指针：缓冲区既泄漏又被 free 两次。
 * 现在改为函数内部局部变量，线程安全。
 */
#define web_log(N, M, ...)                                                   \
        WebLogFmt("[" N " %s:%d] " M, SHORT_FILE, __LINE__, ##__VA_ARGS__)

#endif // !WEB_LOG_H
