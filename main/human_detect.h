/**
 * @file human_detect.h
 * @brief 真实人形（人脸）检测：接 ESP-WHO 的 human_face_detect 模型
 *
 * ★ 与 human_stub 的关系（两者互斥，编译期二选一）：
 *     human_stub.c    : 只打日志的假分数（CONFIG_SNAP_ENABLE_HUMAN_STUB）
 *     human_detect.c  : 本文件，esp-dl 真模型推理（CONFIG_SNAP_ENABLE_HUMAN_DETECT）
 *   后者打开时，snapshot_pipeline 会用它来决定"这张照片要不要上传"。
 *
 * 检测目标其实有两级，模型是**人脸检测**（MSR 粗筛 + MNP 精筛）：
 *     看不到脸  -> 判为"没人"，照片丢弃（只记日志）
 *     看到脸    -> 判为"有人"，入队上传 / 落盘
 *   注意它**不做身份识别**，也不做活体防照片 —— 只回答"画面里有没有人脸"。
 *
 * 输入为什么必须是"打包好的 RGB565"：
 *   esp32-camera 给的是 JPEG，dl 的 ImagePreprocessor 不接受 JPEG，所以本模块
 *   内部把 JPEG 解码成 RGB565 再检测 —— 见 human_detect.c 里的长注释。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 检测结果（供日志 / meta 使用，不含任何身份信息） */
typedef struct {
    bool     human;        /**< true = 画面里有人脸 */
    int      score;        /**< 0..100，最高那张脸的置信度（仅用于展示） */
    int      num_faces;    /**< 检出的人脸数 */
    int      infer_ms;     /**< 本次推理耗时 */
    int      decode_ms;    /**< JPEG 解码耗时 */
} human_detect_result_t;

/**
 * @brief 加载模型（可重复调用，幂等）
 *
 * 模型常驻外部 flash 的 "human_face_det" 分区（见 partitions.csv），
 * 由 human_face_detect 组件在 build 时 pack + flash 时烧写，
 * 加载时按需 mmap，不占用应用分区空间。
 *
 * @param timeout_ms 等待模型就绪的最长时间（内部是懒加载，首次调用会真正加载）
 * @retval ESP_ERR_TIMEOUT 超时（多数情况是模型没烧进 flash 分区）
 */
esp_err_t human_detect_init(int timeout_ms);

/** 是否已就绪（模型加载完成 + 还没被释放） */
bool human_detect_is_ready(void);

/**
 * @brief 对一帧 JPEG 做人脸检测
 *
 * ★ 阻塞函数：内部要解码 + 跑两次推理（实测百毫秒量级），
 *   必须在 trigger 任务上下文调用，绝不能在 httpd / esp_timer 回调里调。
 *
 * @param jpeg  JPEG 数据（参数用 void* 是为了让调用方不必依赖 esp_camera.h）
 * @param len   字节数
 * @param out   输出结果（可为 NULL）
 * @return true = 检测到人（等价于 out->human）
 * @note  检测不可用时**返回 true**（fail-open）：门控的目的是省流量，
 *        不该因为模型故障就把照片全丢掉，宁可多发几张。
 */
bool human_detect_run(const void *jpeg, size_t len, human_detect_result_t *out);

/**
 * @brief ★ 直灌检测：把外部传入的 JPEG 当作一帧来检测（调试用，不接相机）
 *
 * 存在的唯一目的：**把"光学/相机通道"与"算法链路"彻底隔离**。
 * 做法与库尔德语唤醒工程里"wav 直灌"完全等价 ——
 * 真实链路  OV3660 -> JPEG -> sw_decode_jpeg -> ImagePreprocessor -> MSR/MNP
 * 直灌链路  PC 图片 -> (同一份 sw_decode_jpeg) -> (同一个 s_detector) -> MSR/MNP
 * 两条路只在"光 -> 电"这一段不同，**算法链路是同一段代码**。
 *
 * 因此结论是硬的：
 *   · 灌一张确定含脸的图能检出 ⇒ 设备算法/模型/量化全对，问题在相机或拍摄条件；
 *   · 灌进去也检不出     ⇒ 问题就在算法链路本身（与相机无关，别再折腾镜头）。
 *
 * @note 与 human_detect_run() 共用内部实现，唯一差别是 JPEG 来源。
 *       同样阻塞（百毫秒量级），不要在 httpd 回调里直接调 —— 见 web 端的用法。
 */
bool human_detect_run_raw(const void *jpeg, size_t len, human_detect_result_t *out);

/**
 * @brief 释放模型占用的内存
 *
 * 模型本身常驻 flash，但 esp-dl 会把权重/中间张量展开到 PSRAM，
 * 实测占 700KB+。释放后下次 run() 会自动重新加载。
 * 供"内存紧张时让路"或 web 接口调用，正常流程不需要。
 */
void human_detect_release(void);

/** 累计统计（供 /api/status） */
uint32_t human_detect_run_count(void);     /**< 累计检测次数（含失败）      */
uint32_t human_detect_hit_count(void);     /**< 累计判定为"有人"的次数      */
uint32_t human_detect_err_count(void);     /**< 累计因错误走 fail-open 的次数 */

#ifdef __cplusplus
}
#endif
