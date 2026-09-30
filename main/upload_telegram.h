/**
 * @file upload_telegram.h
 * @brief Telegram sendPhoto + 远程命令（任务书 5.6）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief sendPhoto + caption(meta 摘要)
 *
 * 限流：同一 chat 5s 内只发一次（任务书 5.6）。被限流时返回 ESP_ERR_INVALID_STATE
 * 并打 [TG ] throttled 日志。
 * token 只打前 6 位 + "***"。
 *
 * @param buf      JPEG 数据；NULL 表示从 SD 读
 * @param rel_path SD 相对路径
 */
esp_err_t upload_telegram_send_photo(const char *meta_json, const uint8_t *buf,
                                     size_t len, const char *rel_path);

/** 最近错误 */
const char *upload_telegram_last_error(void);

/**
 * @brief 拉取并处理 getUpdates：/snap /status /arm /disarm /reboot
 * @note  由 web_server 或独立 task 周期调用；本函数自带 offset 记忆。
 *        网络失败只打日志并返回，绝不死等。
 */
esp_err_t upload_telegram_poll_commands(void);

/**
 * @brief 轮询已完成的次数（单调递增）
 *
 * 定时唤醒路径用它判断"这一轮 getUpdates 收完了没有"：app_main 起
 * tg_poll_task 后等本计数推进，再决定回睡。
 *
 * ★ app_main **不能**自己去跑 upload_telegram_poll_commands()：
 *   它的栈只有 8KB，TLS + esp_http_client + cJSON 会爆栈 panic 复位
 *   （实测 backtrace 落在 _get_host_header，之后 rst:0xc 反复重启）。
 */
int upload_telegram_poll_done_count(void);

/** 供 /api/status 显示的 bot 用户名（未获取到时为 ""） */
const char *upload_telegram_bot_name(void);

#ifdef __cplusplus
}
#endif
