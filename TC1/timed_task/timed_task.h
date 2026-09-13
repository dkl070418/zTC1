#pragma once
#include <time.h>
#include <stdbool.h>

struct TimedTask;
typedef struct TimedTask* pTimedTask;
struct TimedTask
{
    bool on_use;     //正在使用
    time_t prs_time; //被执行的格林尼治时间戳
    int operation;  //要进行的操作
    int on;          //开或者关，或者其他操作
    /* 0不重复 1-7每周固定日 8每天 9工作日(一~五) 10周末(六~日) */
    int weekday;
    pTimedTask next; //下一个任务(按之间排序)
};

void TaskSubsysInit(void);
pTimedTask NewTask();
bool AddTask(pTimedTask task);
bool DelTask(int time);
/* 按 timed_tasks[] 槽位下标删除（稳定唯一，不随时间戳变） */
bool DelTaskById(int id);
bool DelFirstTask();
void ProcessTask();
char* GetTaskStr();
