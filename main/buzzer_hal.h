/**
 * @file buzzer_hal.h
 * @brief 蜂鸣器（PIN_BUZZER，高电平响）
 *
 * 依据（不要让蜂鸣器成为"只有宏定义没有实现"的空壳，见 docs/wiring.md §用户外设）：
 *   docs/sd_modes.md —— SD **REQUIRED** 模式挂载失败视为致命，
 *                       表现为"**红灯常亮 + 蜂鸣器**"，只保留 safe web 配网，不拍照。
 *   红灯由 led_hal 的 LED_ST_FATAL 表达；蜂鸣器由本模块提供，
 *   并由 led_hal 在进入/退出 FATAL 时联动开关，保证两者语义永远一致。
 *
 * ★ 报警用"周期短鸣"（默认 响 120ms / 停 880ms）而不是长鸣：
 *   长鸣在密闭外壳里发热且扰民，周期短鸣同样能表达"设备故障"。
 *
 * ★ 本模块是可选外设：板子没焊蜂鸣器时，init 失败只打警告，绝不阻断启动。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 GPIO + 起蜂鸣器任务。失败不致命（无声音提示而已） */
esp_err_t buzzer_hal_init(void);

/**
 * @brief 直接开关（仅用于测试/自检）
 *
 * ⚠️ 不要用它做业务提示：本模块的空闲任务每 100ms 会把引脚拉低一次
 *    （BUZZER_IDLE_MS），所以 set(true) 后最多 100ms 就会被关掉，
 *    它做不到"响固定时长"。需要响 N 毫秒请用 buzzer_hal_beep()。
 */
esp_err_t buzzer_hal_set(bool on);

/**
 * @brief 鸣叫指定的毫秒数（非阻塞，由蜂鸣器任务计时）
 *
 * 与 buzzer_hal_alert() 的优先级关系：**alert 优先**。
 *   · 若当前正在 FATAL 报警，本调用被忽略（故障提示不能被业务事件盖掉）；
 *     返回 ESP_ERR_INVALID_STATE。
 *   · 若已有一次 beep 在进行，新的调用会**覆盖**剩余时长（取新的 ms）。
 *
 * @param ms 鸣叫时长，0 表示立即停止。上限 60000ms（防呆）
 * @retval ESP_ERR_NOT_SUPPORTED 板子没焊蜂鸣器 / 未初始化
 */
esp_err_t buzzer_hal_beep(uint32_t ms);

/**
 * @brief 鸣叫"图案"：count 次短鸣，每次 on_ms，两次之间静音 gap_ms（非阻塞）
 *
 * ★ 为什么需要它：buzzer_hal_beep() 是**覆盖**语义（以最后一次调用为准），
 *   连着调两次只会响"一次长的"，做不出业务上要的"滴滴"两短声。
 *   等价关系：buzzer_hal_beep(ms) == buzzer_hal_beep_pattern(1, ms, 0)。
 *
 * 典型用法（"滴 滴"）：buzzer_hal_beep_pattern(2, 80, 120);
 *
 * 优先级同 beep：FATAL 报警期间本调用被忽略并返回 ESP_ERR_INVALID_STATE。
 * 反复调用同样以最后一次为准（会重新从第一次开始响）。
 *
 * @param count 鸣叫次数，<=0 表示立即停止；上限 BUZZER_PATTERN_MAX(10)
 * @param on_ms 每次鸣叫时长，上限 60000ms
 * @param gap_ms 间隔时长，上限 60000ms
 * @retval ESP_ERR_NOT_SUPPORTED 板子没焊蜂鸣器 / 未初始化
 */
esp_err_t buzzer_hal_beep_pattern(int count, uint32_t on_ms, uint32_t gap_ms);

/** 当前 beep 是否还在进行中（含"滴滴"未响完的情况） */
bool buzzer_hal_is_beeping(void);

/**
 * @brief 报警模式开关（非阻塞，可反复调用）
 * @param on true = 开始周期短鸣；false = 立即静音
 */
void buzzer_hal_alert(bool on);

/** 当前是否处于报警短鸣状态 */
bool buzzer_hal_is_alerting(void);

#ifdef __cplusplus
}
#endif
