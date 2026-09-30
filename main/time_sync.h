/**
 * @file time_sync.h
 * @brief SNTP 时间同步（任务书 5.1 第 7 步）
 *
 * 失败时 time_valid=false 但继续运行 —— 此时文件路径用 boot 相对时间命名，
 * 保证 SD 上仍能落盘、且 meta 里明确标注时间不可信。
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 应用 TZ、启动 SNTP、注册同步回调。非阻塞：失败时 time_valid 保持 false。 */
esp_err_t time_sync_init(void);

/** 时间是否已同步过（至少成功一次） */
bool time_sync_valid(void);

/** 触发一次主动同步（web/OTA 前可调用） */
void time_sync_request(void);

/** 供日志用：把 TZ 字符串返回 */
const char *time_sync_tz(void);

#ifdef __cplusplus
}
#endif
