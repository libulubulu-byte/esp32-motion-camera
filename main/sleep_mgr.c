/**
 * @file sleep_mgr.c
 * @brief 深睡 / 唤醒策略实现（设计说明见 sleep_mgr.h）
 *
 * 关键实现点（每一条都对应一个真实会踩的坑）：
 *  1) **显式保电 RTC 外设域**：EXT0/EXT1 的 RTC IO 都在 ESP_PD_DOMAIN_RTC_PERIPH
 *     里，深睡时该域默认会断电。IDF 只在"检测到 EXT0/GPIO"时才自动打开
 *     （见 sleep_modes.c 的 AUTO 分支），**EXT1 不在其列**。所以我们显式打开，
 *     不依赖内部 auto 规则。代价：睡眠电流从 ~10µA 档升到 百µA 档。
 *  2) **睡眠期间的上下拉必须用 RTC 版本**：数字上下拉在深睡会掉电。若 GPIO0
 *     （BOOT0，低有效）悬空被读成低电平，EXT0 会立刻触发 → 醒来就往回睡、
 *     立刻又醒，**形成唤醒风暴、永远睡不着**。所以入睡前显式
 *     pull-up(BOOT0) / pull-down(PIR)。
 *  3) **入睡前等唤醒脚回到非触发电平**：否则同样的道理，armed 的瞬间就满足
 *     触发条件。最多等 SLEEP_IDLE_WAIT_MS，等不到也照睡并告警。
 *  4) **硬兜底**：任何一次唤醒都可能卡在 WiFi/上传上，没有兜底就永远醒着。
 */

#include "sleep_mgr.h"
#include "config_store.h"
#include "logger.h"
#include "pins_config.h"
#include "led_hal.h"       /* 睡前熄灯：WS2812 自锁存，必须显式刷黑 */
#include "camera_hal.h"    /* 睡前关补光 MOS */
#include "esp_camera.h"    /* 睡前让传感器软件掉电（见 sleep_mgr_sleep_now 的 0b 段） */
#include "trigger_fsm.h"   /* 兜底宽限要用 trigger_fsm_state() 判断"是否在抓拍" */
#include "upload_queue.h"  /* 兜底宽限要用 upload_queue_len() 判断"是否在上传" */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_timer.h"

/* 复用 LOG_T_SYS：休眠属于"系统级"事件，不为它新增 TAG
 * （新增 TAG 必须同步改 tools/parse_log.py 的已知集合）。 */
static const char *TAG = LOG_T_SYS;

/* 入睡前的收尾时间：把日志推完、让在飞的 socket 关掉 */
#define SLEEP_SETTLE_MS        300
/* 入睡前等待唤醒脚回到"非触发电平"的最长时间（防唤醒风暴） */
#define SLEEP_IDLE_WAIT_MS     5000
#define SLEEP_IDLE_POLL_MS     50
/* 兜底看门狗任务栈 */
#define SLEEP_GUARD_STACK      3072
/* 兜底的检查粒度（毫秒）：太小费 CPU，太大则"到点"不准 */
#define SLEEP_GUARD_POLL_MS    500
/* 到点后为"正在跑的业务"再宽限的时长（毫秒）。
 * ★ 必须有上限：兜底的意义是"绝不永远醒着"，不能被业务无限拖延。 */
#define SLEEP_GUARD_GRACE_MS   15000

static sleep_wake_t   s_wake = SLEEP_WAKE_COLD;
static volatile bool  s_in_progress;    /* 已在入睡流程中，防重入 */

static const char *wake_str(sleep_wake_t w)
{
    switch (w) {
    case SLEEP_WAKE_PIR:    return "PIR";
    case SLEEP_WAKE_BUTTON: return "BOOT0";
    case SLEEP_WAKE_TIMER:  return "TIMER";
    case SLEEP_WAKE_OTHER:  return "OTHER";
    case SLEEP_WAKE_COLD:
    default:                return "COLD";
    }
}

bool sleep_mgr_enabled(void)
{
    return config_store_get()->sleep_enable != 0;
}

sleep_wake_t sleep_mgr_wake_cause(void) { return s_wake; }
const char  *sleep_mgr_wake_str(void)   { return wake_str(s_wake); }
bool sleep_mgr_is_cold_boot(void)       { return s_wake == SLEEP_WAKE_COLD; }

/* ---------------------------------------------------------------------- */
/*                       入睡前的保护：等空闲电平                          */
/* ---------------------------------------------------------------------- */

/**
 * 等两个唤醒脚回到"非触发电平"，最多 SLEEP_IDLE_WAIT_MS。
 *
 * PIR 高有效 → 要等到低；BOOT0 低有效 → 要等到松开（高）。
 * 不等的话，armed 的瞬间就满足触发条件，芯片会立刻再醒来。
 */
static void wait_wake_pins_idle(void)
{
    int64_t t0 = esp_timer_get_time() / 1000;
    bool pir_low = false, btn_high = false;

    for (;;) {
        pir_low  = (gpio_get_level(PIN_PIR) == 0);
        btn_high = (gpio_get_level(PIN_BUTTON) != 0);
        if (pir_low && btn_high) {
            return;
        }
        if ((esp_timer_get_time() / 1000 - t0) >= SLEEP_IDLE_WAIT_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SLEEP_IDLE_POLL_MS));
    }
    /* 等不到也照睡（业务不能被无限拖住），但必须留证据：
     * 下一次"入睡后立刻醒"的现象就能用这行解释。 */
    LOGW(TAG, "唤醒脚仍在触发电平 (PIR高=%d BOOT0低=%d) -> 照常入睡，可能被立即再次唤醒",
         (int)!pir_low, (int)!btn_high);
}

/* ---------------------------------------------------------------------- */
/*                                  入睡                                   */
/* ---------------------------------------------------------------------- */

void sleep_mgr_sleep_now(const char *reason)
{
    config_store_t *cfg = config_store_get();

    if (!cfg->sleep_enable) {
        LOGI(TAG, "sleep_enable=0 -> 保持常驻不清醒 (reason=%s)", reason ? reason : "?");
        return;
    }
    if (s_in_progress) {
        return;                      /* 已有其它路径在睡，不重入 */
    }
    s_in_progress = true;

    /* 0) ★★ 睡前熄灭**所有**灯（2026-09-28：电池 3.3V 供电，深睡电流偏大）★★
     *
     * 关键在于 **WS2812(GPIO48)**：它内部**自锁存**最后收到的颜色，深睡时
     *   IDF 的"GPIO 隔离"把引脚变高阻，但灯珠照样按旧颜色亮着耗电
     *   （满亮可达 ~20mA，低亮度也有几 mA）—— 这是深睡功耗里最容易漏掉的大头。
     *   必须显式刷一帧"全黑"才能真正灭掉。
     *
     * ★ 位置：必须在下面 SLEEP_SETTLE_MS 的收尾延时**之前**调用。
     *   led_hal 的渲染是在它自己的任务里做的，先 set_state 再留 300ms，
     *   灯才有时间真的灭掉；放到延时之后调，很可能还没渲染完就断电了。
     *
     * 其余三路：绿灯(GPIO2，低有效) / 补光 MOS(GPIO21，高有效) /
     *   蜂鸣器(GPIO47，高有效)。深睡时它们会被 GPIO 隔离拉成高阻而自然熄灭，
     *   这里仍显式配成"灭"的电平 —— 属于保险，万一以后关了 GPIO 隔离，
     *   或者有别的任务在睡前的最后一刻又把它们点亮了，也不会常亮。 */
    led_hal_set_state(LED_ST_OFF);      /* WS2812 刷黑 + 绿灯灭 */
    camera_hal_flash(false);            /* 补光白光 LED 灭 */
    gpio_set_level(PIN_LED_GREEN, 1);   /* active_low：高 = 灭 */
    gpio_set_level(PIN_FLASH, 0);       /* N-MOS：低 = 灭 */
    gpio_set_level(PIN_BUZZER, 0);      /* active_high：低 = 灭 */

    /* 0b) ★★ 让摄像头传感器**软件掉电**（2026-09-28 实测驱动）★★
     *
     * 实测：带摄像头深睡 ~31mA，拔掉摄像头 0.1mA —— 说明深睡时传感器
     *   其实还在跑（OV3660 工作态就是 30~40mA 量级，standby 只有百 µA）。
     *
     * 为什么只剩"软件"这一条路：这块板 CAM_PIN_PWDN / CAM_PIN_RESET 都没引到
     *   GPIO（pins_config.h 里明确配 -1），模组供电又直接挂在 3.3V 域、
     *   路径上没有负载开关 —— 没有硬件手段可以断电。
     *
     * OV3660 / OV5640 系列：寄存器 0x3008 的 bit6 = Software Power Down。
     *     写 0x42 = 掉电（bit6=1），写 0x02 = 恢复。
     * ★ 不需要"醒来再恢复"：深睡唤醒是**完整重启**，摄像头会重新初始化。
     * ★ 写失败也照睡（best effort）—— 只是省不到那 30mA，不影响功能。 */
    /* ★★ 睡前把摄像头软件关进低功耗 —— **由配置开关控制，默认关** ★★
     *
     * ⚠️ 这块板上的真实风险（2026-09-28 实测）：摄像头模组 3.3V 是常电，
     *   CAM_PIN_RESET / CAM_PIN_PWDN 又都没引到 GPIO，所以一旦关进去，
     *   **只有彻底断电才能救回来**（软件清 bit6、deinit + 重新 init 都试过，无效）。
     *   所以默认 0：避免"一睡就再也拍不了照"。
     *
     * 打开方式（无需重烧）：配网页，或 python _cfgset.py cam_powerdown_on_sleep=1
     * 打开前请先确认 camera_hal_init() 里的唤醒逻辑真能让它恢复出帧。 */
    if (config_store_get()->cam_powerdown_on_sleep) {
        sensor_t *s = esp_camera_sensor_get();
        if (s && s->set_reg) {
            int r = s->set_reg(s, 0x3008, 0xFF, 0x42);   /* bit6=1 -> 软件掉电 */
            LOGI(TAG, "camera sw-powerdown 0x3008=0x42 -> %d", r);
        } else {
            LOGW(TAG, "拿不到 sensor 句柄 -> 跳过摄像头低功耗");
        }
    }

    /* 1) 收尾 */
    vTaskDelay(pdMS_TO_TICKS(SLEEP_SETTLE_MS));

    /* 2) 确保唤醒脚处于非触发电平（防唤醒风暴，见文件头第 3 条） */
    wait_wake_pins_idle();

    /* 3) 睡眠期间的上下拉（必须 RTC 版：数字上下拉深睡会掉电）
     *    · PIR  空闲低（高有效）→ 拉低，防悬空飘高自唤醒
     *    · BOOT0 空闲高（低有效）→ 拉高，按下才是低
     *    ★ BOOT0 这条最关键：GPIO0 悬空若被判低，EXT0 立刻触发，
     *      会出现"刚睡就醒、永远睡不着"。 */
    rtc_gpio_pulldown_en(PIN_PIR);
    rtc_gpio_pullup_dis(PIN_PIR);
    rtc_gpio_pullup_en(PIN_BUTTON);
    rtc_gpio_pulldown_dis(PIN_BUTTON);

    /* 4) 注册三个唤醒源 */
    esp_err_t e1 = esp_sleep_enable_ext1_wakeup(1ULL << PIN_PIR,
                                                ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_err_t e0 = esp_sleep_enable_ext0_wakeup(PIN_BUTTON, 0);
    if (e1 != ESP_OK || e0 != ESP_OK) {
        /* 最常见的两种失败：不是 RTC GPIO（0x102），或两个源冲突（0x103） */
        LOGW(TAG, "唤醒源注册异常: ext1(PIR)=%s ext0(BOOT0)=%s -> 仍入睡，但可能醒不过来",
             esp_err_to_name(e1), esp_err_to_name(e0));
    }
    if (cfg->sleep_poll_ms > 0) {
        /* ★ IDF 5.5 的函数名是 esp_sleep_enable_timer_wakeup；
         *   老教程里的 esp_deep_sleep_enable_timer_wakeup 已不存在（编译不过）。 */
        esp_sleep_enable_timer_wakeup((uint64_t)cfg->sleep_poll_ms * 1000ULL);
    }

    /* 5) 显式保电 RTC 外设域（见文件头第 1 条） */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    LOGI(TAG, "进入 deep sleep: reason=%s 唤醒源=PIR(GPIO%d,高)/BOOT0(GPIO%d,低)/%s",
         reason ? reason : "?", PIN_PIR, PIN_BUTTON,
         cfg->sleep_poll_ms > 0 ? "定时" : "定时已关");

    esp_deep_sleep_start();          /* 不返回 */
}

/* ---------------------------------------------------------------------- */
/*                        业务完成 -> 主动入睡                             */
/* ---------------------------------------------------------------------- */

void sleep_mgr_notify_idle(void)
{
    if (!sleep_mgr_enabled()) {
        return;
    }
    sleep_mgr_sleep_now("work_done");
}

/* ---------------------------------------------------------------------- */
/*                          硬兜底看门狗任务                               */
/* ---------------------------------------------------------------------- */

/**
 * 从**本次唤醒**起算的绝对上限：到点无条件睡。
 *
 * 没有它的话，一次 WiFi 关联失败或上传卡死就能让设备永远醒着，
 * 电池几小时耗尽 —— 这是"事件驱动"架构里最容易忘、后果最严重的兜底。
 */
static void sleep_guard_task(void *arg)
{
    (void)arg;
    const int max_ms = config_store_get()->sleep_max_awake_ms;
    if (max_ms <= 0) {
        vTaskDelete(NULL);
        return;
    }
    /* ★★ 2026-09-28 修：到点时若业务**正在跑**必须宽限，不能一刀切 ★★
     *
     * 现场（真机日志）：BOOT0 唤醒后设备本就醒着等按键 —— 这条路没有
     *   "干完活就睡"的事件，全靠本兜底入睡。用户在 30 秒边界上按了一下，
     *   同一瞬间打出：
     *       [PIPE ] grab start flash=300ms retake=on ref=0
     *       [SYS W] 醒着已满 30000ms -> 硬兜底强制入睡
     *   抓拍刚起步就被睡死，照片直接丢 —— 表现为"上传不了图片"。
     *
     * 宽限判据（都是"确实有活在跑"，不是猜）：
     *   · 触发状态机处于 CAPTURE / ENQUEUE（正在抓拍或入队）
     *   · 上传队列非空（正在上传）
     * 宽限有硬上限 SLEEP_GUARD_GRACE_MS：跑完就睡，跑不完也强睡 ——
     *   兜底的本意是"绝不永远醒着"，不能被业务无限拖延。 */
    int waited = 0;
    const int hard_ms = max_ms + SLEEP_GUARD_GRACE_MS;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SLEEP_GUARD_POLL_MS));
        waited += SLEEP_GUARD_POLL_MS;

        if (waited < max_ms) {
            continue;                       /* 还没到点 */
        }
        if (waited >= hard_ms) {
            LOGW(TAG, "业务仍在跑但已到宽限上限 %dms -> 强制入睡", hard_ms);
            break;
        }
        trig_state_t st = trigger_fsm_state();
        if (st != TRIG_CAPTURE && st != TRIG_ENQUEUE && upload_queue_len() == 0) {
            break;                          /* 到点且确实没活了 -> 可以睡 */
        }
    }

    LOGW(TAG, "醒着已满 %dms -> 硬兜底强制入睡", waited);
    sleep_mgr_sleep_now("max_awake_timeout");
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/*                                  初始化                                 */
/* ---------------------------------------------------------------------- */

esp_err_t sleep_mgr_init(void)
{
    config_store_t *cfg = config_store_get();

    /* 1) 认出本次唤醒来源 */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1:      s_wake = SLEEP_WAKE_PIR;    break;
    case ESP_SLEEP_WAKEUP_EXT0:      s_wake = SLEEP_WAKE_BUTTON; break;
    case ESP_SLEEP_WAKEUP_TIMER:     s_wake = SLEEP_WAKE_TIMER;  break;
    case ESP_SLEEP_WAKEUP_UNDEFINED: s_wake = SLEEP_WAKE_COLD;   break;   /* 上电/复位 */
    default:                         s_wake = SLEEP_WAKE_OTHER;  break;
    }

    /* 2) 把 GPIO0/GPIO1 从"RTC 功能"交还给普通 GPIO 驱动。
     *    深睡唤醒后这两个脚仍挂在 RTC IO 上，不 deinit 的话
     *    button_hal / trigger_fsm 的 gpio_config 可能配不上，
     *    现场表现为"PIR 和按键都没反应"。 */
    rtc_gpio_deinit(PIN_PIR);
    rtc_gpio_deinit(PIN_BUTTON);

    LOGI(TAG, "wake=%s (cause=%d) sleep_en=%d poll=%dms max_awake=%dms",
         wake_str(s_wake), (int)cause,
         (int)cfg->sleep_enable, cfg->sleep_poll_ms, cfg->sleep_max_awake_ms);

    /* 3) 硬兜底看门狗：冷启动和唤醒**一样要挂**。
     *    ⚠️ 副作用要讲清楚：上电/烧录后如果不动它，设备最长 sleep_max_awake_ms
     *       就会自己睡过去、网页失联。调试前先 POST /api/config {"sleep_enable":0}。 */
    if (sleep_mgr_enabled() && cfg->sleep_max_awake_ms > 0) {
        if (xTaskCreate(sleep_guard_task, "slp_guard", SLEEP_GUARD_STACK,
                        NULL, 2, NULL) != pdPASS) {
            LOGW(TAG, "兜底任务创建失败 -> 一旦卡住将不会自动入睡");
        }
    }
    return ESP_OK;
}
