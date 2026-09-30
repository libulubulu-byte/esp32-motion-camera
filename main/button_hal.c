/**
 * @file button_hal.c
 * @brief 按键消抖 + 短按/长按识别
 *
 * 用 gpio 中断 + 20ms 轮询消抖。不用 esp_timer 是因为长按需要"到点即触发"，
 * 轮询实现更简单可靠且可读性好（20ms 周期对 CPU 负担可忽略）。
 */

#include "button_hal.h"
#include "pins_config.h"
#include "logger.h"
#include "sleep_mgr.h"   /* ★ 被 BOOT0 唤醒时要跳过启动静默窗口、放宽短按判定 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_LED;

#define POLL_MS        20
#define DEBOUNCE_MS    20

static button_cb_t s_cb;
static void       *s_user;
static volatile bool s_pressed;

static void button_task(void *arg)
{
    (void)arg;

    bool     stable = false;          /* 消抖后的稳定电平 */
    int64_t  press_start = 0;
    bool     long_fired = false;

    /* ★ BOOT0(GPIO0) 是 strapping 脚：启动早期 ROM/bootloader 会采样它，
     *   这段时间内的电平不是"用户按键"，必须整体屏蔽（见 button_hal.h 的说明）。
     *   注意这里**不能 vTaskDelay 一大段**就完事 —— 那期间按下松开会被漏掉；
     *   但本窗口只有 2s 且是启动自检期，用户不会在这时按键，简单延迟最稳。 */
    int64_t boot_ts = esp_timer_get_time() / 1000;
    bool    ignore_until_boot = true;
    int     ignored_events = 0;

    /* ★ 2026-09-28（深睡形态）：被 BOOT0 唤醒时，两件事都要特殊处理 ——
     *   ① **跳过启动静默窗口**：静默窗口本意是防"上电/复位瞬间 GPIO0 的残留
     *      电平被误判成一次短按"；但深睡下 BOOT0 正是唤醒源 —— 用户就是
     *      **按着它**把设备叫醒的，照搬 2s 静默会把这次按键整段吞掉。
     *   ② **放宽"唤醒后第一次松手"的短按判定**：唤醒要重跑 bootloader +
     *      各驱动初始化（约 2~4s），用户从按下到松手**天然超过**
     *      SHORT_PRESS_MAX_MS(2000ms)，正好落进"2s~5s 无效区间" →
     *      原逻辑什么都不做，现场表现为"按醒了但没反应"。
     *      这里只对唤醒后的**第一次**松手放宽，不影响后续正常按键。 */
    bool relax_first_press = (sleep_mgr_wake_cause() == SLEEP_WAKE_BUTTON);
    if (relax_first_press) {
        ignore_until_boot = false;
        stable = (gpio_get_level(PIN_BUTTON) == 0);
        s_pressed = stable;
        if (stable) {
            press_start = esp_timer_get_time() / 1000;
        }
        LOGI(TAG, "BOOT0 唤醒 -> 跳过启动静默窗口；本次松手一律按“人脸识别”处理");
    }

    for (;;) {
        /* ---- 启动屏蔽窗口：期间只采样、不产生事件 ---- */
        if (ignore_until_boot) {
            int64_t elapsed = esp_timer_get_time() / 1000 - boot_ts;
            if (elapsed < BUTTON_BOOT_IGNORE_MS) {
                /* 若此刻电平是"按下"，记一次，出窗口后打日志说明被吞了一次，
                 * 否则用户会以为"按键坏了"（这是 BOOT0 方案最常见的困惑）。 */
                if (gpio_get_level(PIN_BUTTON) == 0) {
                    ignored_events = 1;
                }
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                continue;
            }
            ignore_until_boot = false;
            /* 出窗口时把状态对齐到当前实际电平，否则会把"窗口期内一直按着"
             * 误判成一次"刚按下"（press_start 从出窗口时刻算起，时长虚短）。 */
            stable = (gpio_get_level(PIN_BUTTON) == 0);
            s_pressed = stable;
            if (stable) {
                /* 手还按着：当作正在按下处理，但不认这次短按 */
                press_start = esp_timer_get_time() / 1000;
                long_fired = false;
            }
            if (ignored_events) {
                LOGW(TAG, "boot window (%dms) 内检测到按下 -> 已忽略（BOOT0 上电静默）",
                     BUTTON_BOOT_IGNORE_MS);
            }
            LOGI(TAG, "button ready after boot window: gpio=%d pressed=%d",
                 PIN_BUTTON, (int)stable);
        }

        int raw = gpio_get_level(PIN_BUTTON);
        /* PIN_BUTTON 低有效（内部上拉，按下接地） */
        bool now = (raw == 0);

        if (now != stable) {
            /* 需要连续 DEBOUNCE_MS 保持一致才认 */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if ((gpio_get_level(PIN_BUTTON) == 0) == now) {
                stable = now;
                s_pressed = now;
                if (now) {
                    press_start = esp_timer_get_time() / 1000;
                    long_fired = false;
                } else if (!long_fired) {
                    int64_t held = esp_timer_get_time() / 1000 - press_start;
                    /* relax_first_press：BOOT0 唤醒后的第一次松手，即使超过
                     * SHORT_PRESS_MAX_MS 也按短按处理（原因见任务开头注释）。 */
                    if (held < SHORT_PRESS_MAX_MS || relax_first_press) {
                        LOGI(TAG, "button short press held=%lldms -> 人脸识别（有脸才上传）%s",
                             (long long)held, relax_first_press ? " [BOOT0 唤醒放宽]" : "");
                        if (s_cb) {
                            s_cb(BTN_EV_SHORT_PRESS, s_user);
                        }
                    }
                    relax_first_press = false;   /* 只放宽唤醒后的第一次 */
                }
            }
        }

        /* 长按到点即触发（不需要等松手，用户体验更好；且只触发一次） */
        if (stable && !long_fired) {
            int64_t held = esp_timer_get_time() / 1000 - press_start;
            if (held >= BUTTON_LONG_PRESS_MS) {
                long_fired = true;
                relax_first_press = false;   /* 已判为长按，短按放宽作废 */
                LOGW(TAG, "button long press %lldms -> factory reset",
                     (long long)held);
                if (s_cb) {
                    s_cb(BTN_EV_LONG_PRESS, s_user);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t button_hal_init(button_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        /* ★ BOOT0(GPIO0) 是 strapping 脚：内部上拉**必须常开**，绝不能配上拉的同时
         *   又开下拉（分压可能把复位时的采样电平拖到阈值以下 -> 芯片进 UART 下载
         *   模式，表现为"烧完不跑、串口只打 ROM 横幅"）。所以下面 pull_down 显式
         *   DISABLE，别改成 ENABLE。 */
        .pull_up_en = GPIO_PULLUP_ENABLE,      /* 按键一端接地，靠内部上拉拉高 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(button_task, "button", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    LOGI(TAG, "button init ok gpio=%d BOOT0 (active_low, short=<%dms->人脸识别, long=%dms->恢复出厂, boot_ignore=%dms)",
         PIN_BUTTON, SHORT_PRESS_MAX_MS, BUTTON_LONG_PRESS_MS, BUTTON_BOOT_IGNORE_MS);
    return ESP_OK;
}

bool button_hal_is_pressed(void)
{
    return s_pressed;
}
