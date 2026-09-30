/**
 * @file led_hal.c
 * @brief GPIO2 绿灯 + GPIO48 WS2812(RMT) 语义指示
 *
 * 关键实现决策：
 *  1) WS2812 用 espressif/led_strip 的 RMT 后端（任务书要求 RMT 驱动）。
 *  2) 单独一个 task 以 50ms 周期刷新，业务线程只写状态变量 —— 避免
 *     `led_strip_refresh()`（RMT 事务，内部有信号量等待）被业务线程同步调用。
 *  3) WS2812 初始化失败不应让整机起不来（有些板子可能没焊），降级为只驱动 GPIO2。
 */

#include "led_hal.h"
#include "pins_config.h"
#include "logger.h"
#include "buzzer_hal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = LOG_T_LED;

/* ------------------------------------------------------------------------- */
/* GPIO2 极性：Freenove ESP32-S3-WROOM 的 LED_ON 是"接 3V3 经限流电阻到 GPIO2"，
 * 即 GPIO2 输出低电平点亮。若你的板子相反，把下面这个宏改成 1 即可。 */
#define LED_GREEN_ACTIVE_HIGH  0
#if LED_GREEN_ACTIVE_HIGH
#define LED_GREEN_ON_LEVEL     1
#define LED_GREEN_OFF_LEVEL    0
#else
#define LED_GREEN_ON_LEVEL     0
#define LED_GREEN_OFF_LEVEL    1
#endif

#define LED_TICK_MS            50    /* 刷新周期，20fps */

/* ------------------------------- 内部状态 ------------------------------- */
static led_strip_handle_t s_strip;
static bool               s_strip_ok;
static volatile led_state_t s_state = LED_ST_OFF;
static volatile uint32_t  s_fatal_mask;

/* 双闪序列状态 */
static int      s_dual_flash_left;

/* 蜂鸣器联动的"上一拍"状态：只在 FATAL 进入/退出时翻转，避免每 50ms 重复设置 */
static bool     s_buzzer_alert_on;

/* ------------------------------ WS2812 基础 ------------------------------ */
static void strip_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip_ok) {
        return;
    }
    /* 亮度直接由 RGB 值表达（不做额外缩放），保证"绿=纯绿"语义清晰 */
    if (led_strip_set_pixel(s_strip, 0, r, g, b) != ESP_OK) {
        return;
    }
    led_strip_refresh(s_strip);
}

static void strip_clear(void)
{
    if (!s_strip_ok) {
        return;
    }
    led_strip_clear(s_strip);
}

static void green_set(bool on)
{
    gpio_set_level(PIN_LED_GREEN, on ? LED_GREEN_ON_LEVEL : LED_GREEN_OFF_LEVEL);
}

/* ------------------------------ 语义 → 波形 ------------------------------ */

/**
 * @brief 按 LED 语义计算当前这一 tick 应显示的内容
 * @param t_ms 自启动以来的毫秒数
 */
static void render(led_state_t st, int64_t t_ms, uint8_t *r, uint8_t *g, bool *green_on)
{
    *r = 0;
    *g = 0;
    *green_on = false;

    switch (st) {
    case LED_ST_OFF:
    case LED_ST_COOLDOWN:
        break;

    case LED_ST_BOOT: {                        /* 红快闪 250ms */
        bool on = ((t_ms / 125) & 1) == 0;
        if (on) {
            *r = 32;
        }
        break;
    }

    case LED_ST_FATAL:                         /* 红常亮 */
        *r = 48;
        break;

    case LED_ST_SD_DEGRADE: {                  /* 红慢闪 1s */
        bool on = ((t_ms / 500) & 1) == 0;
        if (on) {
            *r = 32;
        }
        break;
    }

    case LED_ST_READY: {                       /* 绿慢闪 2s：亮 100ms 灭 1900ms */
        int64_t ph = t_ms % 2000;
        if (ph < 100) {
            *g = 24;
        }
        break;
    }

    case LED_ST_CAPTURE: {                     /* 绿快闪 200ms */
        bool on = ((t_ms / 100) & 1) == 0;
        if (on) {
            *g = 40;
        }
        break;
    }

    case LED_ST_UPLOAD_OK: {                   /* 绿双闪：两次 120ms 亮 / 120ms 灭 */
        static const int64_t seq[] = {0, 120, 240, 360};   /* 亮区间起点 */
        const int64_t total = 480;
        int64_t ph = t_ms % total;
        bool on = false;
        for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
            if (ph >= seq[i] && ph < seq[i] + 120) {
                on = true;
                break;
            }
        }
        if (on) {
            *g = 40;
        }
        *green_on = on;
        break;
    }
    }

    /* GPIO2 随"绿状态"同步：任何时刻只要绿分量 > 0 就点亮 GPIO2 */
    if (*g > 0) {
        *green_on = true;
    }
}

/* ------------------------------- LED 任务 ------------------------------- */
static void led_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time() / 1000;

    for (;;) {
        led_state_t st = s_state;
        int64_t t_ms = esp_timer_get_time() / 1000 - t0;

        /* ★ FATAL 联动蜂鸣器。
         *   docs/sd_modes.md 规定 SD REQUIRED 挂载失败 = "红灯常亮 + 蜂鸣器"，
         *   两条指示必须同进同出。放在这里统一表达，避免以后只设 LED 忘开蜂鸣器
         *   （也顺带覆盖 camera 初始化失败 —— 它同样走 led_hal_set_fatal_or）。 */
        {
            bool want = (st == LED_ST_FATAL) || (s_fatal_mask != 0);
            if (want != s_buzzer_alert_on) {
                s_buzzer_alert_on = want;
                buzzer_hal_alert(want);
            }
        }

        uint8_t r = 0, g = 0;
        bool green_on = false;
        render(st, t_ms, &r, &g, &green_on);

        if (r || g) {
            strip_set_rgb(r, g, 0);
        } else {
            strip_clear();
        }
        green_set(green_on);

        /* 双闪是一次性的：跑完一圈自动回到 READY（若有致命错误则回 FATAL） */
        if (st == LED_ST_UPLOAD_OK) {
            s_dual_flash_left--;
            if (s_dual_flash_left <= 0) {
                s_state = (s_fatal_mask != 0) ? LED_ST_FATAL : LED_ST_READY;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

/* -------------------------------- 对外 API -------------------------------- */
esp_err_t led_hal_init(void)
{
    /* GPIO2 直驱 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LED_GREEN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    green_set(false);

    /* WS2812 (GPIO48) —— 失败只降级，不阻断启动 */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_WS2812,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10MHz → 0.1us 分辨率 */
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        s_strip_ok = false;
        LOGW(TAG, "ws2812 init fail err=0x%x -> gpio2 only", err);
    } else {
        s_strip_ok = true;
        led_strip_clear(s_strip);
    }

    BaseType_t ok = xTaskCreate(led_task, "led", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        LOGE(TAG, "led task create fail");
        return ESP_ERR_NO_MEM;
    }
    LOGI(TAG, "init ok ws2812=gpio%d(%s) green=gpio%d(active_%s)",
         PIN_LED_WS2812, s_strip_ok ? "rmt" : "off",
         PIN_LED_GREEN, LED_GREEN_ACTIVE_HIGH ? "high" : "low");
    return ESP_OK;
}

void led_hal_set_state(led_state_t st)
{
    if (st == LED_ST_UPLOAD_OK) {
        s_dual_flash_left = 1;      /* 至少跑完一轮 480ms 序列 */
    }
    s_state = st;
}

void led_hal_set_fatal_or(uint32_t fatal_mask)
{
    s_fatal_mask |= fatal_mask;
    s_state = LED_ST_FATAL;
}

void led_hal_clear_fatal(uint32_t fatal_mask)
{
    s_fatal_mask &= ~fatal_mask;
    if (s_fatal_mask == 0 && s_state == LED_ST_FATAL) {
        s_state = LED_ST_READY;
    }
}

bool led_hal_is_fatal(void)
{
    return s_fatal_mask != 0;
}

led_state_t led_hal_get_state(void)
{
    return s_state;
}
