/**
 * @file buzzer_hal.c
 * @brief 蜂鸣器驱动：GPIO 直驱 + 独立任务产生周期短鸣
 *
 * 设计要点：
 *  1) 单独一个 task 产生鸣叫波形，业务线程只写 s_alert 标志 ——
 *     与 led_hal.c / button_hal.c 的做法一致，避免业务线程被延时阻塞。
 *  2) GPIO 配置失败 / 任务创建失败都只降级（无声音），绝不阻断启动：
 *     蜂鸣器是可选外设，有些板子根本没焊。
 *  3) 复用 LOG_T_LED（"LED"）：蜂鸣器与 LED 同属"设备指示"，
 *     不为它新增 TAG —— 新增 TAG 必须同步改 tools/parse_log.py 的已知集合。
 */

#include "buzzer_hal.h"
#include "pins_config.h"
#include "logger.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"       /* esp_timer_get_time()：beep 到期时刻 */

static const char *TAG = LOG_T_LED;

/* 报警波形：响 120ms / 停 880ms（1s 周期）。改这里即可调整手感。 */
#define BUZZER_ON_MS    120
#define BUZZER_OFF_MS   880
/* 未报警时的巡检周期，决定"关"的响应延迟 */
#define BUZZER_IDLE_MS  100

/* beep 上限：防呆用，避免调用方传错单位（例如把秒当毫秒传了 2）导致长鸣 */
#define BUZZER_BEEP_MAX_MS  60000
/* "滴滴"图案最多几次，防呆（避免调用方传个 1000 次，变成扰民长鸣） */
#define BUZZER_PATTERN_MAX  10

static volatile bool     s_alert;

/* ---- beep / 图案状态机 ----
 * 用「状态 + 绝对到期时刻 + 剩余次数」表达，而不是「剩余时长递减」：
 *   · 以绝对时刻为准 ⇒ 多次调用天然表现为"以最后一次为准"；
 *   · 再加一个剩余次数 ⇒ 才能表达"滴滴"这种多次短鸣。
 *     （原来只有单次覆盖语义，连调两次只会响一次长的，做不出两短声。） */
typedef enum {
    BZ_IDLE = 0,    /* 静音 */
    BZ_ON,          /* 正在响 */
    BZ_GAP,         /* 两次短鸣之间的静音间隔 */
} bz_state_t;

static volatile bz_state_t s_state;
static volatile int64_t    s_deadline_ms;   /* 当前状态结束的绝对时刻（ms） */
static volatile int        s_repeat;        /* 本次 ON 结束后还要再响几次 */
static volatile uint32_t   s_on_ms;         /* 单次鸣叫时长 */
static volatile uint32_t   s_gap_ms;        /* 间隔时长 */
#if PIN_BUZZER >= 0
static bool          s_gpio_ok;      /* 同样受 #if 保护，避免 PIN_BUZZER<0 时"定义未使用" */
#endif

#if PIN_BUZZER >= 0
/* ★ 这两个静态函数也必须包在同一个 #if 里：若有人把 PIN_BUZZER 配成负数，
 *   它们会变成"定义了但未使用"，在 -Werror 下直接把构建打断。 */

static void buzzer_write(bool on)
{
    if (!s_gpio_ok) {
        return;
    }
    gpio_set_level(PIN_BUZZER, on ? 1 : 0);
}

static void buzzer_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* ★ 优先级：FATAL 报警 > 一次性 beep。
         *   报警代表"设备故障"，绝不能被业务事件鸣叫盖掉，
         *   否则用户会失去"靠声音判断故障"的能力。 */
        if (s_alert) {
            buzzer_write(true);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_ON_MS));
            buzzer_write(false);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_OFF_MS));
            continue;
        }

        int64_t now = esp_timer_get_time() / 1000;

        switch (s_state) {
        case BZ_ON:
            buzzer_write(true);
            if (now >= s_deadline_ms) {
                /* 本次响完：还有剩余次数就进间隔，否则收工 */
                buzzer_write(false);
                if (s_repeat > 0) {
                    s_repeat--;
                    s_state = BZ_GAP;
                    s_deadline_ms = now + s_gap_ms;
                } else {
                    s_state = BZ_IDLE;
                }
            } else {
                /* 每次最多睡 50ms：既能精确收尾，又能及时响应 alert 抢占
                 * （若直接睡满剩余时长，报警最长会被延迟到 beep 结束） */
                int64_t left = s_deadline_ms - now;
                vTaskDelay(pdMS_TO_TICKS(left < 50 ? left : 50));
            }
            break;

        case BZ_GAP:
            buzzer_write(false);
            if (now >= s_deadline_ms) {
                s_state = BZ_ON;
                s_deadline_ms = now + s_on_ms;
            } else {
                int64_t left = s_deadline_ms - now;
                vTaskDelay(pdMS_TO_TICKS(left < 50 ? left : 50));
            }
            break;

        case BZ_IDLE:
        default:
            buzzer_write(false);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_IDLE_MS));
            break;
        }
    }
}
#endif /* PIN_BUZZER >= 0 */

esp_err_t buzzer_hal_init(void)
{
#if PIN_BUZZER >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        LOGW(TAG, "buzzer gpio%d config fail err=0x%x -> 无声音提示", PIN_BUZZER, err);
        return err;
    }
    gpio_set_level(PIN_BUZZER, 0);      /* 上电保持静音 */
    s_gpio_ok = true;

    if (xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 3, NULL) != pdPASS) {
        LOGW(TAG, "buzzer task create fail -> 无声音提示");
        s_gpio_ok = false;
        return ESP_ERR_NO_MEM;
    }

    LOGI(TAG, "buzzer init ok gpio=%d (active_high, alert=%dms/%dms)",
         PIN_BUZZER, BUZZER_ON_MS, BUZZER_OFF_MS);
    return ESP_OK;
#else
    LOGI(TAG, "buzzer disabled (PIN_BUZZER<0)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t buzzer_hal_set(bool on)
{
#if PIN_BUZZER >= 0
    if (!s_gpio_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(PIN_BUZZER, on ? 1 : 0);
    return ESP_OK;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void buzzer_hal_alert(bool on)
{
    s_alert = on;
    if (on) {
        /* 进报警时立刻取消未完成的 beep/图案，两者的"当前状态"不应叠加 */
        s_state  = BZ_IDLE;
        s_repeat = 0;
    }
}

bool buzzer_hal_is_alerting(void)
{
    return s_alert;
}

esp_err_t buzzer_hal_beep(uint32_t ms)
{
    /* 单次短鸣 = "只有一次"的图案：两者共用同一套状态机，
     * 不会再出现"beep 与 pattern 互相打架"的情况。 */
    return buzzer_hal_beep_pattern(1, ms, 0);
}

esp_err_t buzzer_hal_beep_pattern(int count, uint32_t on_ms, uint32_t gap_ms)
{
#if PIN_BUZZER >= 0
    if (!s_gpio_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* count<=0 或 on_ms==0：立即停（与 beep(0) 的语义保持一致） */
    if (count <= 0 || on_ms == 0) {
        s_state  = BZ_IDLE;
        s_repeat = 0;
        buzzer_write(false);
        return ESP_OK;
    }
    if (count > BUZZER_PATTERN_MAX) {
        LOGW(TAG, "beep 次数 %d 超上限 -> 截到 %d", count, BUZZER_PATTERN_MAX);
        count = BUZZER_PATTERN_MAX;
    }
    if (on_ms > BUZZER_BEEP_MAX_MS) {
        LOGW(TAG, "beep %ums 超上限 -> 截到 %dms", (unsigned)on_ms, BUZZER_BEEP_MAX_MS);
        on_ms = BUZZER_BEEP_MAX_MS;
    }
    if (gap_ms > BUZZER_BEEP_MAX_MS) {
        LOGW(TAG, "beep 间隔 %ums 超上限 -> 截到 %dms", (unsigned)gap_ms, BUZZER_BEEP_MAX_MS);
        gap_ms = BUZZER_BEEP_MAX_MS;
    }
    if (s_alert) {
        /* 报警优先：业务事件不能盖掉故障提示 */
        LOGW(TAG, "beep %d 次被忽略（正在 FATAL 报警）", count);
        return ESP_ERR_INVALID_STATE;
    }

    /* 绝对到期时刻 + 剩余次数：重复调用天然表现为"以最后一次为准" */
    s_on_ms       = on_ms;
    s_gap_ms      = gap_ms;
    s_repeat      = count - 1;      /* 本次 ON 之外还要再响几次 */
    s_deadline_ms = esp_timer_get_time() / 1000 + (int64_t)on_ms;
    s_state       = BZ_ON;
    return ESP_OK;
#else
    (void)count;
    (void)on_ms;
    (void)gap_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool buzzer_hal_is_beeping(void)
{
    return s_state != BZ_IDLE;
}
