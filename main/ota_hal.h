/**
 * @file ota_hal.h
 * @brief OTA（esp_https_ota + MD5 + 回滚）（任务书 5.9）
 *
 * 约束：
 *  - OTA 之前必须停 MJPEG stream（本工程 stream 默认关；开启时由 web_server 负责）
 *  - MD5 校验失败 -> 不写分区 -> 不 reboot -> 不 bootloop
 *  - 结果写 NVS；下次启动打印 [OTA] last_result=ok/fail
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** OTA 结果（持久化在 NVS） */
typedef enum {
    OTA_RES_NONE = 0,
    OTA_RES_OK,
    OTA_RES_FAIL,
} ota_result_t;

/** 启动时调用：读 NVS 并打印 [OTA] last_result=... */
void ota_hal_report_last_result(void);

/** 上一次 OTA 结果 */
ota_result_t ota_hal_last_result(void);

/**
 * @brief 执行一次 OTA
 * @param url        固件 URL（http/https）
 * @param md5_hex    期望 MD5（32 hex），为空则跳过本地校验
 * @param stop_stream_cb 回调：在开始下载前调用，用于停掉 stream 等资源
 * @return ESP_OK = 升级成功（已重启或即将重启）
 * @note  本函数**阻塞**，应在独立任务里调用（web handler 会创建临时任务）。
 *        失败时不重启，直接返回错误。
 */
typedef void (*ota_prepare_cb_t)(void);
esp_err_t ota_hal_perform(const char *url, const char *md5_hex, ota_prepare_cb_t prepare_cb);

/**
 * @brief 在独立任务里跑 OTA（web handler 用，避免阻塞 httpd）
 * @param out_msg 结果描述（调用方提供栈上缓冲，函数返回后才可读）
 */
esp_err_t ota_hal_perform_async(const char *url, const char *md5_hex);

/** 是否正在 OTA（/api/status 用） */
bool ota_hal_in_progress(void);

/** 最近一次错误描述 */
const char *ota_hal_last_error(void);

#ifdef __cplusplus
}
#endif
