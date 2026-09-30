/**
 * @file sleep_mgr.h
 * @brief 深睡 / 唤醒策略（2026-09-28 新增）
 *
 * ---------------------------------------------------------------------------
 * 设计（需求：默认深睡 + 定时唤醒；PIR / BOOT0 / 定时器 三个唤醒源）
 * ---------------------------------------------------------------------------
 * 唤醒源分工：
 *   · **PIR（EXT1, GPIO1, 高有效）** —— 人来了立刻醒 → 拍照 → 识别 → 上传
 *   · **BOOT0（EXT0, GPIO0, 低有效）** —— 按键唤醒 → 走按键语义（短按人脸识别）
 *   · **定时器（每 sleep_poll_ms）** —— 只轮询一次 Telegram 命令，让
 *     /snap /status 这类远程命令在"常睡"形态下依然可用
 *
 * 三段式生命周期：
 *   ① sleep_mgr_init()      醒来后最先调用：认出唤醒源 + 挂上硬兜底看门狗
 *   ② （业务跑）             拍照/识别/上传；冷启动还要做 OTA 检查
 *   ③ sleep_mgr_notify_idle() 或看门狗超时  → sleep_mgr_sleep_now() 入睡
 *
 * ⚠️ 为什么必须有"硬兜底"：任何一次唤醒如果卡在 WiFi 关联或上传上，
 *   没有兜底就会**永远醒着**，电池几小时就耗光。sleep_max_awake_ms 是
 *   从醒来起算的绝对上限，到点无条件睡。
 *
 * ⚠️ 每次唤醒都是**完整重启**（重跑 bootloader + 重连 WiFi + 重初始化
 *   摄像头），所以"唤醒→照片到达"的真实延迟是 5~8s 量级，不是毫秒级。
 *
 * ⚠️ 日志环形缓冲在 RAM 里，深睡会丢 —— 醒来后 /api/log 是空的，
 *   想看跨睡眠的历史日志必须用串口常接。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本次启动的唤醒来源 */
typedef enum {
    SLEEP_WAKE_COLD = 0,    /**< 上电/复位/烧录 —— 走完整启动（含 OTA 检查） */
    SLEEP_WAKE_PIR,         /**< EXT1：PIR 触发 */
    SLEEP_WAKE_BUTTON,      /**< EXT0：BOOT0 按下 */
    SLEEP_WAKE_TIMER,       /**< 定时器：只需轮询一次 Telegram */
    SLEEP_WAKE_OTHER,       /**< 其它（理论上不该出现） */
} sleep_wake_t;

/**
 * @brief 醒来后**尽早**调用（config_store_init 之后、各驱动 init 之前）
 *
 * 做的事：① 读并打印唤醒来源；② 释放可能残留的 RTC IO 配置（把 GPIO1/GPIO0
 * 交还给普通 GPIO 驱动）；③ 若配置允许，挂上 sleep_max_awake_ms 硬兜底。
 */
esp_err_t sleep_mgr_init(void);

sleep_wake_t sleep_mgr_wake_cause(void);
const char  *sleep_mgr_wake_str(void);

/** 是否冷启动（上电/复位）。冷启动才做上电 OTA 检查等一次性动作 */
bool sleep_mgr_is_cold_boot(void);

/** 深睡总开关当前是否生效（读配置，可运行时改） */
bool sleep_mgr_enabled(void);

/**
 * @brief 通知"本次业务已做完，可以睡了"
 *
 * 适用于：判定无人丢弃、上传成功/失败且队列已空。
 * 内部会先给一小段 settle 时间（让日志/连接收尾）再入睡。
 * 配置 sleep_enable=0 时本函数直接返回（设备保持常驻）。
 */
void sleep_mgr_notify_idle(void);

/**
 * @brief 立即注册唤醒源并进入 deep sleep（**不返回**）
 *
 * 先把两个唤醒脚等到"非触发电平"再睡（否则会立刻自唤醒、形成唤醒风暴），
 * 再显式保电 RTC 外设域（EXT0/EXT1 都依赖它），最后 esp_deep_sleep_start()。
 *
 * 配置 sleep_enable=0 时本函数只打一行日志并返回，不做任何事。
 *
 * @param reason 日志用原因串（如 "work_done" / "max_awake_timeout"）
 */
void sleep_mgr_sleep_now(const char *reason);

#ifdef __cplusplus
}
#endif
