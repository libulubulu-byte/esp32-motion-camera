/**
 * @file button_hal.h
 * @brief 按键 = 板载 BOOT0 (GPIO0)：短按进入人脸识别（检测到人脸才上传），长按 5s 恢复出厂
 *
 * 事件通过回调抛给业务层（trigger_fsm / main），button_hal 本身不做业务决策。
 *
 * ★ 2026-09-28 两处变更（引脚 14 -> 0 / 短按语义改了）：
 *   1. 引脚改到 BOOT0：见 pins_config.h 里 PIN_BUTTON 的注释（串扰/strapping 约束）。
 *   2. 短按不再是"无条件拍照上传"，而是**送进人脸门控**：
 *      抓拍 -> human_detect 判有没有人脸 -> 有脸才入队上传，无脸只在串口记一行。
 *      判定逻辑复用 snapshot_pipeline 的 human_gate_take()，button_hal 不知情、
 *      只负责把 BTN_EV_SHORT_PRESS 抛出去，所以将来改回"无条件拍照"只需改上层。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_LONG_PRESS_MS  5000     /* 任务书：长按 5s 恢复出厂 */
#define SHORT_PRESS_MAX_MS    2000     /* 松手时长 < 2s 才算短按（长按路径另算） */

/**
 * 上电后忽略按键的窗口（毫秒）。
 *
 * ★ 为什么需要：BOOT0 = GPIO0，ROM/bootloader 在启动早期会采样这个脚。
 *   实测若不屏蔽，**上电瞬间手还搭在键上**（或板子释放电平有拖尾）会被判成
 *   一次短按，出现"一上电先自己拍一张"。2s 足够覆盖 bootloader + 外设初始化。
 */
#define BUTTON_BOOT_IGNORE_MS 2000

typedef enum {
    BTN_EV_SHORT_PRESS = 0,   /**< 松手且按下时长 < SHORT_PRESS_MAX_MS -> 人脸识别 */
    BTN_EV_LONG_PRESS,        /**< 按住到 5s（松手前就会触发一次）-> 恢复出厂    */
} button_event_t;

typedef void (*button_cb_t)(button_event_t ev, void *user);

/**
 * @brief 初始化按键
 * @param cb   事件回调（在 button task 上下文里调用，禁止阻塞 >10ms）
 * @param user 回调透传参数
 * @note  引脚用 PIN_BUTTON，内部上拉 + 低有效；20ms 消抖。
 */
esp_err_t button_hal_init(button_cb_t cb, void *user);

/** 按下电平（1 = 当前按住） */
bool button_hal_is_pressed(void);

#ifdef __cplusplus
}
#endif
