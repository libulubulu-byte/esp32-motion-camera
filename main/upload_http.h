/**
 * @file upload_http.h
 * @brief HTTP multipart/form-data 上传（任务书 5.6）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief POST multipart/form-data：meta(JSON) + file(JPEG)
 *
 * 字段名：meta / file；文件名固定 snapshot.jpg
 * Header：X-Device-Key: <config.http_header_value>（若已配置）
 * 超时 10s；重试 3 次，退避 1/2/4s；仅 200/201/202 视为成功。
 *
 * @param meta_json  meta 事件 JSON
 * @param buf        JPEG 数据（buffer 模式）；为 NULL 表示从 SD 读
 * @param len        JPEG 长度
 * @param rel_path   SD 相对路径（path 模式）；为 NULL 表示用 buf
 */
esp_err_t upload_http_send(const char *meta_json, const uint8_t *buf, size_t len,
                           const char *rel_path);

/** 最近一次失败原因（简短字符串，给 /api/status.last_error） */
const char *upload_http_last_error(void);

#ifdef __cplusplus
}
#endif
