/**
 * @file ota_check.h
 * @brief 上电自动升级：GET 版本清单 -> 比版本 -> 决定是否升级
 *
 * 与 ota_hal 的分工：
 *   ota_hal   = "怎么升级"（下载、校验、写分区、重启）—— 被动，等一个 URL
 *   ota_check = "要不要升级"（拿清单、比版本）—— 主动，上电跑一次
 *
 * 手动升级（POST /api/ota 带 url）走 ota_hal，不经过这里；
 * 两条路径互不冲突，但都会置 ota_hal 的 in_progress 标志，不会并发。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 上电检查一次版本，需要则升级（**阻塞**，内含下载全过程）
 *
 * 行为：
 *   - APP_OTA_CHECK_URL 为空 -> 直接返回 ESP_OK（功能关闭，不打日志）
 *   - 取清单失败 / 解析失败 / 版本不高于本机 -> 返回 ESP_OK（这不算错误）
 *   - 清单版本更高 -> 调 ota_hal_perform 下载并升级；成功则重启，不会返回
 *   - 升级失败 -> 返回错误，但**设备正常继续运行**（ota_hal 保证不重启）
 *
 * @return ESP_OK      无需升级，或已成功升级（不会返回）
 *         ESP_FAIL    拿不到清单 / 清单格式不对 / 升级失败（详见日志）
 *
 * @note 必须放在**独立任务**里调用，或在 app_main 末尾（业务已启动后）调用。
 *       原因：取清单是网络操作，最长会阻塞 APP_OTA_CHECK_TIMEOUT_MS；
 *       若放在 MQTT 启动之前，服务器不在线时会拖慢首连十几秒。
 *
 * @note 不要在 SD REQUIRED 的 safe mode 早退**之后**调用 —— 那正是
 *       "卡坏了需要靠 OTA 修" 的场景，早退会让设备永远升不了级。
 */
esp_err_t ota_check_on_boot(void);

#ifdef __cplusplus
}
#endif
