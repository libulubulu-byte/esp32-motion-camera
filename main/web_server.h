/**
 * @file web_server.h
 * @brief 内置 web server + mDNS（任务书 5.7）
 *
 * 端点总览（全部 JSON 都是 `Content-Type: application/json; charset=utf-8`）：
 *   GET  /                     -> web/index.html（内嵌到固件的单页控制台）
 *   GET  /api/status           -> 设备状态（5.7 规定的全部字段）
 *   POST /api/snapshot         -> 立刻拍一张（受冷却/armed 限制，可 force=1 绕过）
 *   GET  /api/last_snapshot    -> 最近一张 JPEG（SD 优先，降级用 PSRAM）
 *   GET  /api/photos           -> 文件列表（SD_OK 才有效）
 *   GET  /api/events           -> 最近 50 条事件（内存环形表）
 *   GET  /api/log              -> 最近 80 行日志（内存环形缓冲）
 *   GET  /api/config           -> 脱敏后的配置
 *   POST /api/config           -> 改配置（部分键即可）
 *   POST /api/arm              -> 布防
 *   POST /api/disarm           -> 撤防
 *   GET  /api/sd               -> SD 状态
 *   POST /api/sd/remount       -> 手动重挂载
 *   GET  /api/stream           -> MJPEG（仅 CONFIG_SNAP_ENABLE_STREAM=y，默认关闭）
 *   POST /api/ota              -> 触发 OTA（需 url，可选 md5）
 *   POST /api/reboot           -> 重启
 *   POST /api/factory_reset    -> 恢复出厂设置
 *
 * 鉴权（任务书 5.7）：
 *   - 改配置类（POST /api/config、/api/ota、/api/factory_reset、/api/sd/remount）
 *     必须带 `X-Admin-Key: <admin_key>`，不符返回 401 {"error":"unauthorized"}。
 *   - 只读 / 拍照 / arm 不要求鉴权（局域网内可控，方便配网后直接验证）。
 *
 * mDNS：`esp32s3cam.local`，服务 `_http._tcp`。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_MDNS_HOST   "esp32s3cam"

/** 启动 httpd（端口 80）+ mDNS。失败返回错误（调用方据此进 safe mode） */
esp_err_t web_server_init(void);

/** 是否正在 MJPEG 推流（OTA 前需要停掉；默认编译时 stream 关闭） */
bool web_server_stream_active(void);

/** 停止 MJPEG 推流（OTA 的 prepare 回调里调用） */
void web_server_stream_stop(void);

#ifdef __cplusplus
}
#endif
