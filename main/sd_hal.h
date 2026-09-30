/**
 * @file sd_hal.h
 * @brief 板载 microSD（SDMMC 1-bit）三态 + 状态机（任务书 5.5）
 *
 * 三态宏（编译期，由 Kconfig SD_MODE_xxx 决定）：
 *   NONE     : sd_state=DISABLED，全部接口返回 ESP_ERR_NOT_SUPPORTED
 *   OPTIONAL : mount 失败降级；写失败连续 3 次降级；30s 自动重挂载
 *   REQUIRED : mount 失败视为致命错误（调用方据此进 safe mode）
 *
 * 路径约定：/sdcard/snapshots/YYYY/MM/DD/HHMMSS_mmm.jpg
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_DISABLED = 0,   /**< SD_MODE_NONE 或编译期关掉             */
    SD_UNKNOWN,        /**< 尚未尝试挂载                          */
    SD_MOUNTING,       /**< 正在挂载（含重试）                    */
    SD_OK,             /**< 已挂载且可写                          */
    SD_FAIL,           /**< 挂载/写失败，已降级到 PSRAM 队列       */
    SD_REMOVED,        /**< 运行期检测到卡被拔掉                   */
    SD_LOWSPACE,       /**< 剩余空间 < sd_low_space_mb            */
} sd_state_t;

typedef struct {
    sd_state_t state;
    uint64_t   total_bytes;
    uint64_t   free_bytes;
    char       cid[24];            /**< 格式化后的 CID 前 8 字节 hex */
    uint32_t   retry_count;        /**< 累计重挂载尝试次数            */
    int        last_error;         /**< 最近一次 esp_err_t            */
} sd_status_t;

/* ------------------------------------------------------------------ */
/* 编译期模式查询：让别的模块能写 `if (SD_MODE_IS_NONE)` 而不用 #ifdef  */
bool sd_hal_is_enabled(void);      /**< false = SD_MODE_NONE          */
bool sd_hal_is_required(void);     /**< true  = SD_MODE_REQUIRED      */

/**
 * @brief 初始化并尝试挂载
 * @retval ESP_OK       已挂载（或 NONE 模式下的"正常不可用"）
 * @retval ESP_FAIL     REQUIRED 模式下挂载失败（调用方应进 safe mode）
 */
esp_err_t sd_hal_init(void);

/** 取状态快照 */
sd_status_t sd_hal_status(void);

/** 当前状态（高频调用，无锁读） */
sd_state_t sd_hal_state(void);

/** 人可读状态名（日志 / /api/status） */
const char *sd_hal_state_str(sd_state_t st);

/**
 * @brief 写一个文件（自动建目录），内部带 3 次失败降级逻辑
 * @param rel_path 相对 /sdcard 的路径，如 "snapshots/2026/09/26/192001_012.jpg"
 * @note  内部先写 ".tmp" 再 rename，避免掉电留下半个 jpg。
 * @retval ESP_ERR_NOT_SUPPORTED SD 不可用（调用方应走 PSRAM 分支）
 * @retval ESP_ERR_INVALID_STATE 已降级（同上）
 */
esp_err_t sd_hal_write_file(const char *rel_path, const void *data, size_t len);

/** 读文件，返回实际长度；-1 = 失败 */
int sd_hal_read_file(const char *rel_path, void *buf, size_t buf_sz);

/** 文件是否存在 */
bool sd_hal_file_exists(const char *rel_path);

/** 文件大小；-1 = 失败/不存在 */
long sd_hal_file_size(const char *rel_path);

/** 删除文件；返回 0 成功 */
int sd_hal_remove(const char *rel_path);

/** 手动重挂载（POST /api/sd/remount） */
esp_err_t sd_hal_remount(void);

/**
 * @brief 空间检查 + 清理最旧日期目录
 * @note  **禁止删除未上传成功的文件**。本工程的做法是：只清理
 *        "已被 upload_queue 标记为上传成功"的那个日期目录（详见 .c 注释）。
 * @return 清理后的剩余字节数
 */
uint64_t sd_hal_check_space(void);

/** 把绝对路径 /sdcard/a/b.jpg 转成相对路径 a/b.jpg（并做前缀校验） */
const char *sd_hal_to_rel(const char *abs_path, char *out, size_t out_sz);

/**
 * @brief 通知"这个文件已上传成功"，用于低空间清理水位线
 * @param rel_path 相对路径，如 "snapshots/2026/09/26/192001_012.jpg"
 * @note  低空间清理**只允许删除严格早于该水位的日期目录**，
 *        因此上传失败的文件绝不会被删掉。由 upload_queue 在成功回调里调用。
 */
void sd_hal_note_uploaded_ok(const char *rel_path);

/** 生成 snapshots 下的相对路径 + 对应的 .meta.json 路径 */
void sd_hal_make_paths(char *jpg_rel, size_t jpg_sz, char *meta_rel, size_t meta_sz);

#ifdef __cplusplus
}
#endif
