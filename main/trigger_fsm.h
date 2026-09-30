/**
 * @file trigger_fsm.h
 * @brief 触发状态机（任务书 5.3）
 *
 * IDLE -> DEBOUNCE -> CAPTURE -> ENQUEUE -> COOLDOWN -> IDLE
 * 触发源：pir / button / remote_http / remote_mqtt / timer
 *
 * 可配参数（从 config_store 实时读取，改完立即生效）：
 *   pir_active_level, pir_debounce_ms=300, min_trigger_interval_ms=3000, flash_before_ms=300
 *
 * COOLDOWN 内 PIR 只计数不拍照，日志形如：
 *   [TRIG] cooldown skip count=1
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "upload_queue.h"      /* trig_src_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRIG_IDLE = 0,
    TRIG_DEBOUNCE,
    TRIG_CAPTURE,
    TRIG_ENQUEUE,
    TRIG_COOLDOWN,
} trig_state_t;

typedef enum {
    TRIG_RES_OK = 0,
    TRIG_RES_DISARMED,     /**< armed=false，不拍照 */
    TRIG_RES_COOLDOWN,     /**< 冷却期内 */
    TRIG_RES_BUSY,         /**< 上一次还没走完 */
    TRIG_RES_ERROR,        /**< 拍照失败 */
} trig_result_t;

const char *trig_state_str(trig_state_t st);
const char *trig_result_str(trig_result_t r);

/**
 * @brief 初始化状态机：订阅 PIR / 按键，并启动触发任务
 * @note  PIR 用 GPIO 中断 + 队列（ISR 只投递时间戳，不做任何判断）
 */
esp_err_t trigger_fsm_init(void);

/**
 * @brief 主动触发一次（web / telegram / timer 用）
 * @param src  触发来源（会写进 meta）
 * @param note 备注（日志用，可为 NULL）
 * @return 见 trig_result_t；ESP_OK 表示已受理
 */
esp_err_t trigger_fsm_fire(trig_src_t src, const char *note);

/** 当前状态 */
trig_state_t trigger_fsm_state(void);

/** 是否在冷却中（/api/status 可展示） */
bool trigger_fsm_in_cooldown(void);

/** 累计触发次数（成功进入 CAPTURE 的） */
uint32_t trigger_fsm_count(void);

/** 累计冷却期被跳过的次数（PIR 丢弃计数） */
uint32_t trigger_fsm_cooldown_skips(void);

/** arm/disarm（同步 config 与 FSM 内部的 armed 标志） */
void trigger_fsm_set_armed(bool armed);
bool trigger_fsm_is_armed(void);

/** 恢复出厂：button 长按调用 */
esp_err_t trigger_fsm_factory_reset(void);

#ifdef __cplusplus
}
#endif
