/**
 * @file led_hal.h
 * @brief LED 语义（任务书 5.10）
 *
 *  GPIO48 WS2812 ：红快闪=启动，红常亮=致命错误，红慢闪=SD 降级中，
 *                  绿慢闪=就绪，绿快闪=拍照中，绿双闪=上传成功，灭=COOLDOWN
 *  GPIO2  LED_ON ：随"绿状态"同步（极性由 CONFIG/宏 LED_GREEN_ACTIVE_LEVEL 决定）
 *
 * 实现要点：所有闪烁由 led_hal 自己的一个 task 驱动，业务代码只 set_state()，
 * 不阻塞业务线程。RMT 驱动 WS2812 时不能在高频路径里反复 transmit，
 * 因此刷新周期固定 50ms（=20fps），足以表达所有语义。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_ST_OFF = 0,
    LED_ST_BOOT,        /**< 红快闪（250ms 周期）    */
    LED_ST_FATAL,       /**< 红常亮                  */
    LED_ST_SD_DEGRADE,  /**< 红慢闪（1s 周期）        */
    LED_ST_READY,       /**< 绿慢闪（2s 周期）        */
    LED_ST_CAPTURE,     /**< 绿快闪（200ms 周期）     */
    LED_ST_UPLOAD_OK,   /**< 绿双闪一次后回落         */
    LED_ST_COOLDOWN,    /**< 灭                       */
} led_state_t;

/** 致命错误来源位掩码（可能同时多个） */
#define LED_FATAL_CAMERA    0x01
#define LED_FATAL_SD_REQ    0x02

/**
 * @brief 初始化 GPIO2 与 WS2812(RMT)，并启动 LED 驱动任务
 * @note  WS2812 使用 RMT TX 通道，与 gpio_hal 的 GPIO2 直驱互不干扰。
 *        (RMT 通道与 esp32-camera 的 LCD_CAM 外设无冲突；与 SDMMC 也无冲突。)
 */
esp_err_t led_hal_init(void);

/** 设置 LED 语义状态（非阻塞，立即返回） */
void led_hal_set_state(led_state_t st);

/** 追加一个致命错误来源；状态自动切到 LED_ST_FATAL */
void led_hal_set_fatal_or(uint32_t fatal_mask);

/** 清除一个致命错误来源（全部清空后需要业务自己 set_state 回 READY） */
void led_hal_clear_fatal(uint32_t fatal_mask);

/** 当前是否处于致命错误 */
bool led_hal_is_fatal(void);

/** 当前状态（调试 / /api/status 用） */
led_state_t led_hal_get_state(void);

#ifdef __cplusplus
}
#endif
