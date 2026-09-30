/**
 * @file camera_hal.h
 * @brief 摄像头抽象层：OV2640 / OV3660 / OV5640 编译期切换
 *
 * 统一接口（任务书 5.2）：
 *   cam_init / cam_grab / cam_return / cam_set_param / cam_sensor_name
 * 本工程用官方命名前缀一致的 camera_hal_xxx()。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"          /* framesize_t / pixformat_t */
#include "esp_camera.h"      /* camera_fb_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 可动态调整的摄像头参数（对应任务书 cam_param_t） */
typedef enum {
    CAM_PARAM_FRAMESIZE = 0,
    CAM_PARAM_QUALITY,
    CAM_PARAM_BRIGHTNESS,
    CAM_PARAM_CONTRAST,
    CAM_PARAM_SATURATION,
} cam_param_id_t;

typedef struct {
    cam_param_id_t id;
    int            value;
} cam_param_t;

/**
 * @brief 初始化摄像头（引脚来自 pins_config.h，XCLK 来自 Kconfig）
 *
 * 行为：
 *  - 按 CONFIG_CAMERA_SENSOR_xxx 只填一款 sensor 配置表（不做运行时探测切换）
 *  - frame_size 超上限自动 clamp + 打警告
 *  - 打印 [CAM ] init ok sensor=... pid=... frame=... q=.. xclk=.. fb=2xPSRAM
 * @retval ESP_ERR_NOT_FOUND 传感器握手/读 PID 失败（典型 err=0x105）
 * @retval ESP_ERR_INVALID_SIZE frame buffer 分配失败（PSRAM 不足）
 */
esp_err_t camera_hal_init(void);

/**
 * @brief 取一帧 JPEG —— **保证是"本次调用之后才开始曝光"的那一帧**
 *
 * 驱动只要有空槽就会自己往前采帧（见 `camera_hal.c` 里 grab 段的长注释），
 * 所以"队列里那一张"往往是几秒前就采好的旧帧。本函数按 `fb->timestamp`
 * 把这类旧帧丢掉再取，最多丢 `CAM_GRAB_MAX_DRAIN` 张。
 *
 * @param timeout_ms "丢旧帧"阶段的总时长上限（毫秒），0 = 不限制。
 *                   ⚠️ `esp_camera_fb_get()` 内部另有固定的 4s 超时，本参数管不到它。
 * @return 非 NULL 的 framebuffer（必须 camera_hal_return 归还），失败返回 NULL
 */
camera_fb_t *camera_hal_grab(uint32_t timeout_ms);

/**
 * @brief 归还 framebuffer（唯一释放路径）
 * @note  传入 NULL 是合法的 no-op，方便调用方写 `cam_return(fb)` 而不用判空。
 */
esp_err_t camera_hal_return(camera_fb_t *fb);

/** 运行期改参数（带 clamp 与警告） */
esp_err_t camera_hal_set_param(cam_param_t p);

/**
 * @brief 把运行期配置的帧尺寸 / JPEG 质量应用到传感器
 *
 * 配置保存后（`POST /api/config`）调用一次，让改动**立即生效**、不必重启。
 * 启动路径不经过它 —— `camera_hal_init()` 直接就拿 /config.json 的值初始化了，
 * 因为 framebuffer 是按 init 时的 framesize 分配的（原因见 camera_hal.c 注释）。
 *
 * @note 只会"改小或保持"：改大分辨率不会生效（缓冲区不够，会花屏），
 *       此时函数打警告并提示重启，返回值仍可能为 ESP_OK。
 */
esp_err_t camera_hal_apply_runtime(int framesize, int quality);

/** 当前传感器名，如 "OV2640" */
const char *camera_hal_sensor_name(void);

/** 编译期选定的传感器期望 PID 的字符串形式，如 "0x2642" */
const char *camera_hal_expected_pid_str(void);

/** 实际握手到的 PID（未 init 时为 0） */
uint16_t camera_hal_pid(void);

/** flicker（50/60Hz）设置 */
esp_err_t camera_hal_set_flicker(int hz);

/**
 * @brief 拍照补光：开/关 GPIO21
 * @note  必须在 grab 之前 flash_before_ms 打开、grab 之后立刻关闭，
 *        否则 OV5640 长曝光会过曝。
 */
esp_err_t camera_hal_flash(bool on);

/** 当前帧大小（枚举值），供 /api/status 显示 */
int camera_hal_current_framesize(void);

/**
 * @brief 当前 JPEG 质量 —— **实际生效值**
 *
 * 与 `/api/config` 里的 `jpeg_quality`（**请求值**）可能不同：请求值超出 4..63、
 * 或 init 时被帧尺寸的默认质量覆盖时，两者会不一致。/api/status 报的是这个。
 */
int camera_hal_current_quality(void);

/** 当前帧大小的人可读名，如 "SVGA(800x600)" */
const char *camera_hal_framesize_name(int fs);

#ifdef __cplusplus
}
#endif
