#include "http_server/web_log.h"
#include "TimeUtils.h"

#include "mico.h"
#include "main.h"
#include "mqtt_server/user_mqtt_client.h"
#include "user_power.h"

/* ------------------------------------------------------------------------- *
 * 计量链路：功耗IC的 CF 输出 -> GPIO_0(MICO_GPIO_15) 下降沿中断 -> 计数/测频
 *
 * 唯一的标定量：一个 CF 脉冲代表多少 µWh。
 * 原代码把魔数 17.1 同时当两个用途：
 *     瞬时功率  real_time_power = 17.1 * (脉冲/秒)      —— 单位 0.1W
 *     累计电能  kWh = 17.1 * p_count / 1000 / 36000
 * 两者数值上自洽（17.1/36000 Wh * 3600 s/h * 10 = 17.1），所以单位链是对的，
 * 但这个常数从来没有对照真实负载标过，实测偏差极大（见 POWER_PULSE_UWH 注释）。
 * ------------------------------------------------------------------------- */

/* 1 脉冲 = 475 µWh  == 原 17.1/36000 Wh，保持与旧的能量公式完全一致 */
volatile uint32_t power_pulse_uwh = POWER_PULSE_UWH_DEFAULT;
volatile float power_pulses_per_min = 0;

volatile uint32_t p_count = 0;
PowerRecord power_record = {1, {0}};
char power_record_str[1101] = {0};

/* 单位固定为 0.1W —— 前端 index.html:1260 和 MQTT 都会 /10 还原成 W */
volatile float real_time_power = 0;

/* 脉冲间隔的指数滑动平均（微秒）。在 ISR 里写、1s 定时器里读，用 volatile。 */
static volatile uint32_t interval_ema_us = 0;
static volatile uint64_t irq_last_ns = 0;
/* 只在真正收到 CF 脉冲时更新。不要用 last_pulse_ms 做这件事：
 * PowerReinit() 也会重置它，导致"距上次脉冲多久"被重新武装的时刻污染。 */
static volatile uint32_t last_pulse_ms = 0;
static volatile uint32_t last_true_pulse_ms = 0;
static volatile uint8_t  ever_had_pulse = 0;
static uint32_t p_count_snapshot = 0;

/* 软件轮询交叉验证：用来区分"IC 没出脉冲"和"硬件中断在丢脉冲"。
 * 前者是空载/硬件问题，后者才是固件问题。 */
static volatile uint8_t  power_diag_on = 0;
static volatile uint32_t sw_edge_count = 0;
static volatile uint32_t poll_high = 0;
static volatile uint32_t poll_total = 0;
static volatile uint8_t  poll_last_level = 1;
static mico_timer_t power_poll_timer;
static bool power_poll_created = false;

/* 脉冲率统计：每 POWER_PPM_WINDOW_MS 算一次"脉冲/分钟" */
static uint32_t ppm_base_count = 0;
static uint32_t ppm_base_ms = 0;

static mico_timer_t power_sample_timer;
static bool power_timer_created = false;

void SetPowerRecord(PowerRecord *pr, uint32_t pw) {
    pr->powers[(++pr->idx) % PW_NUM] = pw;
}

char *GetPowerRecord(int idx) {
    /* 前端每次都会请求 power.idx + 1。空载时 power_record.idx 不前进，
     * 于是 idx > power_record.idx 恒成立，原代码直接 return ""，
     * 导致 powers 是空数组、前端不更新数值 —— 面板从此"冻住"。
     * 这里改成钳位到最新一条，保证空载时也能持续输出 0。 */
    if (idx > power_record.idx) idx = power_record.idx;
    if (idx < 0) idx = 0;
    idx = idx <= power_record.idx - PW_NUM ? 0 : idx;

    int i = idx > 0 ? idx : (power_record.idx - PW_NUM + 1);
    i = i < 0 ? 0 : i;
    char *tmp = power_record_str;
    char *end = power_record_str + sizeof(power_record_str);
    for (; i <= power_record.idx; i++) {
        int n = snprintf(tmp, (size_t) (end - tmp), "%lu,",
                         (unsigned long) power_record.powers[i % PW_NUM]);
        if (n < 0 || tmp + n >= end) break;
        tmp += n;
    }
    if (tmp > power_record_str) *(tmp - 1) = 0;   /* 去掉末尾逗号 */
    else *tmp = 0;
    return power_record_str;
}

/*
 * ISR 只做最少的事：计数 + 更新间隔滑动平均。
 *
 * 原实现在 ISR 里做浮点除法、还按 n 回填最多几十条"伪造"的历史采样
 * （for i<n: SetPowerRecord(...)）。实测 60 秒内 p_count 只加了 6，
 * power_record.idx 却加了 27 —— 就是这 21 个凭空气泡，图表因此全是噪声。
 */
static void PowerIrqHandler(void *arg) {
    //警告! 不能在此函数里调用任何有关moloc()的操作
    uint64_t now_ns = mico_nanosecond_clock_value();
    uint64_t gap_ns = now_ns - irq_last_ns;
    uint32_t now_ms = (uint32_t) (mico_rtos_get_time());

    irq_last_ns = now_ns;
    last_pulse_ms = now_ms;
    last_true_pulse_ms = now_ms;
    ever_had_pulse = 1;
    p_count++;

    /* 首次中断或间隔异常大（>60s，说明之前空载）时不更新均值，避免污染 */
    if (gap_ns == 0 || gap_ns > 60000000000ULL) return;

    {
        uint32_t gap_us = (uint32_t) (gap_ns / 1000u);
        uint32_t prev = interval_ema_us;

        interval_ema_us = (prev == 0) ? gap_us : ((prev * 3u + gap_us) / 4u);
    }
}

/*
 * 2ms 软件轮询 CF 引脚，独立于硬件中断数下降沿。
 *
 * 这是判断病根的关键交叉验证：
 *   硬件中断数 << 软件轮询数  ==> 中断在丢脉冲（固件/SDK 问题，可修）
 *   硬件中断数 == 软件轮询数 == 0 且引脚恒高 ==> IC 根本没出脉冲（空载，或 IC/外围硬件故障）
 *   引脚恒低 ==> IC 输出级被拉死或上拉没供电（硬件问题，与固件无关）
 * 只在诊断模式开启时运行，避免常态下每 2ms 调一次 MoC 的 GPIO 读取。
 */
static void PowerPollHandler(void *arg) {
    uint8_t level;

    if (!power_diag_on) return;

    level = MicoGpioInputGet(POWER) ? 1 : 0;
    poll_total++;
    if (level) poll_high++;

    if (poll_last_level && !level) sw_edge_count++;   /* 下降沿 */
    poll_last_level = level;
}

/*
 * 每 1 秒采样一次瞬时功率并写入历史环形缓冲区。
 * 这样图表的横轴是真实时间，而不是"脉冲来了才有格子"。
 */
static void PowerSampleHandler(void *arg) {
    uint32_t ema_us = interval_ema_us;
    uint32_t now_ms = (uint32_t) (mico_rtos_get_time());
    uint32_t elapsed;

    /* 脉冲率统计窗口：这是判断"脉冲在丢"还是"标定常数不对"的关键观测量 */
    elapsed = now_ms - ppm_base_ms;
    if (elapsed >= POWER_PPM_WINDOW_MS) {
        uint32_t delta = p_count - ppm_base_count;

        power_pulses_per_min = (float) delta * 60000.0f / (float) elapsed;
        ppm_base_count = p_count;
        ppm_base_ms = now_ms;
    }

    /* 长时间没有脉冲 == 空载，强制归零，否则 EMA 会把最后一次读数永远冻住 */
    if (ema_us == 0 || (now_ms - last_pulse_ms) > POWER_IDLE_TIMEOUT_MS) {
        interval_ema_us = 0;
        real_time_power = 0;
        SetPowerRecord(&power_record, 0);
        return;
    }

    /* W = 脉冲/秒 * (µWh/脉冲 * 1e-6) Wh * 3600 s/h ；再 *10 转成 0.1W 单位 */
    {
        float pps = 1000000.0f / (float) ema_us;
        float watts = pps * ((float) power_pulse_uwh / 1000000.0f) * 3600.0f;
        uint32_t tenths;

        real_time_power = watts * 10.0f;
        /* 四舍五入而不是截断：原来 (int) 会把 8.6 存成 8，前端 /10 就成了 0.80 */
        tenths = (uint32_t) (watts * 10.0f + 0.5f);
        SetPowerRecord(&power_record, tenths);
    }
}

void PowerInit(void) {
    power_log("PowerInit");

    if (!power_timer_created) {
        mico_rtos_init_timer(&power_sample_timer, 1000, PowerSampleHandler, NULL);
        power_timer_created = true;
    }
    mico_rtos_start_timer(&power_sample_timer);

    PowerReinit();
}

/*
 * 重新武装功耗脉冲输入。
 *
 * POWER = MICO_GPIO_15，在 MK3031 的引脚映射表里对应物理 GPIO_0
 * (mico-os/platform/MCU/MW3xx/peripherals/sdk/src/boards/EMW3031.c:384)，
 * 而 GPIO_0 同时是 SPI0_CLK / UART0_CTS 的复用脚。
 * MicoGpio* 全部转发进闭源 MoC 库(moc_api.c:197)，micoWlanStart() 每次切换
 * Station/SoftAP 都可能重新初始化 MoC 侧的引脚复用。所以每次调用
 * micoWlanStart() 之后都重新武装一次，成本极低且无害。
 */
void PowerReinit(void) {
    MicoGpioInitialize(POWER, INPUT_PULL_UP);
    MicoGpioEnableIRQ(POWER, IRQ_TRIGGER_FALLING_EDGE, PowerIrqHandler, NULL);

    irq_last_ns = mico_nanosecond_clock_value();
    interval_ema_us = 0;
    last_pulse_ms = (uint32_t) mico_rtos_get_time();

    power_log("PowerReinit: CF pulse input re-armed");
}

/* 距上一个真实 CF 脉冲过去了多少毫秒；从未收到过脉冲时返回 0xFFFFFFFF */
uint32_t PowerMsSincePulse(void) {
    if (!ever_had_pulse) return 0xFFFFFFFFu;
    return (uint32_t) mico_rtos_get_time() - last_true_pulse_ms;
}

uint8_t PowerEverHadPulse(void) {
    return ever_had_pulse;
}

/* 开关 2ms 软件轮询交叉验证 */
void PowerDiagSet(uint8_t on) {
    if (on && !power_poll_created) {
        mico_rtos_init_timer(&power_poll_timer, 2, PowerPollHandler, NULL);
        power_poll_created = true;
    }

    if (on) {
        sw_edge_count = 0;
        poll_high = 0;
        poll_total = 0;
        poll_last_level = MicoGpioInputGet(POWER) ? 1 : 0;
        power_diag_on = 1;
        if (!mico_rtos_is_timer_running(&power_poll_timer))
            mico_rtos_start_timer(&power_poll_timer);
        power_log("diag ON: polling CF pin every 2ms");
    } else {
        power_diag_on = 0;
        if (power_poll_created) mico_rtos_stop_timer(&power_poll_timer);
        power_log("diag OFF");
    }
}

/* 把当前交叉验证结果格式化成 "sw_edges_per_min high_pct samples pin" */
void PowerDiagReport(char *out, int out_len) {
    uint32_t hi, tot, edges;
    uint32_t now_ms = (uint32_t) mico_rtos_get_time();
    static uint32_t diag_base_ms = 0;
    static uint32_t diag_base_edges = 0;
    float per_min = 0;

    if (!power_diag_on) {
        snprintf(out, out_len, "OFF");
        return;
    }

    hi = poll_high; tot = poll_total; edges = sw_edge_count;

    if (diag_base_ms == 0) { diag_base_ms = now_ms; diag_base_edges = edges; }
    if (now_ms - diag_base_ms > 3000) {
        per_min = (float) (edges - diag_base_edges) * 60000.0f / (float) (now_ms - diag_base_ms);
        diag_base_ms = now_ms;
        diag_base_edges = edges;
    }

    snprintf(out, out_len, "%.0f %u%% %u",
             (double) per_min,
             (unsigned) (tot ? (hi * 100u / tot) : 0),
             (unsigned) tot);
}

/* 供标定使用：返回自上次调用以来新增的脉冲数 */
uint32_t PowerPulsesSinceLastCall(void) {
    uint32_t now = p_count;
    uint32_t delta = now - p_count_snapshot;

    p_count_snapshot = now;
    return delta;
}

/* 开机时归一化：flash 里这个位置以前是没人用的 last_wifi_status，
 * 老设备上可能是 0 或擦除态 0xFFFFFFFF，必须校验后再用 */
void PowerLoadCalibration(void) {
    int stored = user_config->power_pulse_uwh;

    if (stored < (int) POWER_PULSE_UWH_MIN || stored > (int) POWER_PULSE_UWH_MAX) {
        power_pulse_uwh = POWER_PULSE_UWH_DEFAULT;
        user_config->power_pulse_uwh = (int) POWER_PULSE_UWH_DEFAULT;
        mico_system_context_update(sys_config);
        power_log("calibration absent/invalid in flash, using default %u uWh/pulse",
                  (unsigned) POWER_PULSE_UWH_DEFAULT);
    } else {
        power_pulse_uwh = (uint32_t) stored;
        power_log("calibration loaded: %u uWh/pulse", (unsigned) power_pulse_uwh);
    }
}

uint32_t PowerSetPulseUwh(uint32_t uwh) {
    if (uwh < POWER_PULSE_UWH_MIN) uwh = POWER_PULSE_UWH_MIN;
    if (uwh > POWER_PULSE_UWH_MAX) uwh = POWER_PULSE_UWH_MAX;

    power_pulse_uwh = uwh;
    user_config->power_pulse_uwh = (int) uwh;
    mico_system_context_update(sys_config);

    /* 改了每脉冲能量，历史曲线和瞬时值的单位就变了，清掉重新积累避免新旧混在一起 */
    memset(power_record.powers, 0, sizeof(power_record.powers));
    power_record.idx = 1;
    interval_ema_us = 0;

    power_log("calibration set to %u uWh/pulse", (unsigned) uwh);
    return uwh;
}

/* 按实测脉冲率反算。脉冲率还没统计出来时返回 0，调用方据此提示用户等待 */
uint32_t PowerCalibrateFromWatts(float watts) {
    float ppm = power_pulses_per_min;
    uint32_t uwh;

    if (watts <= 0.0f) return 0;
    /* 脉冲太少时反算误差极大，至少要求窗口内有几个脉冲 */
    if (ppm < 2.0f) {
        power_log("calibrate refused: pulses/min=%.2f too low (wait for the window)", ppm);
        return 0;
    }

    /* uwh = W * 60000000 / pulses_per_min  （W * 3600 s/h * 1e6 µWh/Wh / 60 min/h） */
    uwh = (uint32_t) (watts * 60000000.0f / ppm + 0.5f);
    return PowerSetPulseUwh(uwh);
}
