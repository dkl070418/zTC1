#ifndef __USER_POWER_H_
#define __USER_POWER_H_

#define PW_NUM 100

/*
 * 功率标定：1 个 CF 脉冲代表多少 µWh（微瓦时）。
 *
 * 默认 475 µWh 等价于原代码的魔数 17.1（17.1/36000 Wh = 475 µWh）。
 * 前端"功耗校准"卡片可以直接改，存在 user_config 里，掉电不丢。
 *
 * 标定公式：
 *   uwh = 真实瓦数 W * 60000000 / 实测脉冲每分
 * 例：真实 40W、实测 1404 脉冲/分 -> 40*6e7/1404 = 1709 µWh
 *     真实 40W、实测   6 脉冲/分 -> 40*6e7/6    = 400000 µWh
 */
#define POWER_PULSE_UWH_DEFAULT  475u
/* 合理区间；超出一律回退默认值（含 flash 里的旧数据 / 擦除态 0xFFFFFFFF） */
#define POWER_PULSE_UWH_MIN      1u
#define POWER_PULSE_UWH_MAX      20000000u
/* 连续这么久没有脉冲就判定空载，瞬时功率归零 */
#define POWER_IDLE_TIMEOUT_MS    60000u
/* 脉冲率统计窗口 */
#define POWER_PPM_WINDOW_MS      30000u

typedef struct
{
    int idx;
    uint32_t powers[PW_NUM];
} PowerRecord;

extern PowerRecord power_record;
extern volatile uint32_t p_count;
/* 单位 0.1W —— 前端和 MQTT 都要 /10 才是瓦特 */
extern volatile float real_time_power;
/* 生效的标定值（已做区间校验），单位 µWh/脉冲 */
extern volatile uint32_t power_pulse_uwh;
/* 最近一个统计窗口的脉冲率，已换算成"脉冲/分钟" */
extern volatile float power_pulses_per_min;

extern char* GetPowerRecord(int idx);
extern void PowerInit(void);
/* 每次 micoWlanStart() 之后必须重新调用，否则脉冲中断可能被 MoC 重排引脚时冲掉 */
extern void PowerReinit(void);
extern void SetPowerRecord(PowerRecord* pr, uint32_t pw);
extern uint32_t PowerMsSincePulse(void);
extern uint32_t PowerPulsesSinceLastCall(void);
extern uint8_t  PowerEverHadPulse(void);
/* 2ms 软件轮询交叉验证：区分"IC 没出脉冲"和"硬件中断在丢脉冲" */
extern void PowerDiagSet(uint8_t on);
extern void PowerDiagReport(char *out, int out_len);

/* 开机读取配置后调用，做区间校验并归一 */
extern void PowerLoadCalibration(void);
/* 直接设置标定值并落盘；返回实际生效值（越界会被夹住） */
extern uint32_t PowerSetPulseUwh(uint32_t uwh);
/* 按实测脉冲率反算：watts 为负载真实瓦数；脉冲率无效时返回 0 不改配置 */
extern uint32_t PowerCalibrateFromWatts(float watts);

#endif
