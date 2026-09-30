/**
 * @file mqtt_hal.h
 * @brief MQTT 元数据发布（默认关闭，CONFIG_SNAP_ENABLE_MQTT=n）
 *
 * 任务书要求："默认 n，空实现也须能编"。
 * 因此本模块在关闭时所有函数都是可链接的空实现，调用方无需 #ifdef。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（关闭时返回 ESP_ERR_NOT_SUPPORTED，调用方忽略即可） */
esp_err_t mqtt_hal_init(void);

/** 是否已连接（关闭时恒为 false） */
bool mqtt_hal_connected(void);

/**
 * @brief 只发 meta 到 <前缀>/<12位MAC>/event（前缀与账号见 app_conf.h）
 * @note  断线重连退避 1/2/5/10/30s，**不影响本地拍照与上传链路**：
 *        失败只打日志，绝不阻塞调用者。
 */
esp_err_t mqtt_hal_publish_meta(const char *meta_json);

/** 当前状态字符串，供 /api/status 用；关闭时返回 "off" */
const char *mqtt_hal_state_str(void);

#ifdef __cplusplus
}
#endif
