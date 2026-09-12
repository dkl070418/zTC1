#ifndef __USER_ZCONTROL_H_
#define __USER_ZCONTROL_H_

#include <stdbool.h>

/* ZControl / SmartControl_PC 的 JSON 控制协议（阶段 C）
 * 命令: {"mac":"<12hex>","plug_N":{"on":0|1}}  查询时 on 为 null
 * 状态: 含 mac + plug_0..5{on,setting{name}} + power/version/...
 */
bool ZcHandleCommand(const char *json);
int  ZcBuildStateJson(char *buf, int buflen);
void ZcPublishState(void);

#endif
