/**
 * @file snapshot_pipeline.h
 * @brief 拍照管道：grab -> 分流(SD 落盘 / PSRAM 入队)（任务书 5.4）
 *
 * 唯一允许调用 camera_hal_grab / camera_hal_return 的地方（除 camera_hal 自身）。
 * 保证：每一个 framebuffer 都在本文件里被归还，且只有一条路径。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "upload_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（清状态、打印一次管道配置） */
esp_err_t snapshot_pipeline_init(void);

/**
 * @brief 执行一次完整拍照：补光 -> grab -> 落盘或拷贝 -> 归还 fb -> 入队
 * @param src 触发来源（写进 meta）
 * @return true = 成功入队
 * @note  本函数是**阻塞**的（含 flash_before_ms + grab + 写卡），
 *        必须在 trigger 任务上下文调用，不要在 httpd / LVGL 线程里调。
 */
bool snapshot_pipeline_capture(trig_src_t src);

/**
 * @brief 取"最近一张"照片（给 GET /api/last_snapshot）
 *
 * SD_OK 时返回相对路径（调用方自己去读文件）；
 * 降级时返回内存里的最近一张 JPEG 指针（调用方不要 free）。
 *
 * @param out_path  输出 SD 相对路径（可能为 ""）
 * @param path_sz   out_path 容量
 * @param out_len   输出内存 JPEG 长度（0 = 没有）
 * @return 内存 JPEG 指针，或 NULL
 */
const uint8_t *snapshot_pipeline_last(char *out_path, size_t path_sz, size_t *out_len);

/** 累计统计 */
uint32_t snapshot_pipeline_ok_count(void);
uint32_t snapshot_pipeline_fail_count(void);

/* ---------- 人形门控计数（调试"是不是人才上传"用的唯一远程口径） ----------
 *
 * 语义（三个数加起来才是"抓过几张"）：
 *   run  = 跑过人形检测的次数
 *   hit  = 判为"有人" -> 已入队上传
 *   miss = 判为"没人" -> 已丢弃（不计入 fail_count）
 *   run - hit - miss = 走了 fail-open（检测出错，照片照常上传）
 *
 * ★ 注意 run 数的是**帧**不是**触发次数**：判坏补拍时若补拍帧也被检测，
 *   一次触发可能让 run 加 2（见 snapshot_pipeline.c 里补拍段的说明）。
 *
 * 开关关闭时恒为 0 —— 这本身就是"门控没生效"的判据。 */
uint32_t snapshot_pipeline_human_run_count(void);
uint32_t snapshot_pipeline_human_hit_count(void);
uint32_t snapshot_pipeline_human_miss_count(void);

/** PSRAM 历史最低水位（验收项"连拍 50 次无持续下降"的量化判据） */
uint32_t snapshot_pipeline_psram_min(void);

#ifdef __cplusplus
}
#endif
