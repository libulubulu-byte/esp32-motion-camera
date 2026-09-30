/**
 * @file human_stub.h
 * @brief 人形检测占位实现（任务书 0.11：只允许打印日志的 stub）
 *
 * ★ 明确不实现：身份识别、人脸比对、防照片活体。
 *   本模块只统计"这一帧大概有多大人形可能性"的假分数用于链路演示，
 *   分数完全由 JPEG 体积启发式生成，不代表任何真实检测能力。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>      /* size_t：不能再依赖调用方的包含顺序 */

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（只在 CONFIG_SNAP_ENABLE_HUMAN_STUB=y 时被调用） */
void human_stub_init(void);

/**
 * @brief 对一帧 JPEG 跑"检测"
 * @param jpeg    帧数据
 * @param len     字节数
 * @param out_score 输出假分数 0..100
 * @return true = 认为有人（score >= 阈值）
 * @note  仅在日志层面使用，不会触发任何额外动作，也不会拒绝上传。
 */
bool human_stub_detect(const uint8_t *jpeg, size_t len, int *out_score);

#ifdef __cplusplus
}
#endif
