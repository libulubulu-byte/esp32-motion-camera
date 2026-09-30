/**
 * @file wifi_hal.h
 * @brief Wi-Fi STA + AP 配网 + 断线重连（任务书 5.1 第 6 步）
 *
 * 行为：
 *  - STA 优先（凭据从 config_store 取，未配置则直接进 AP）
 *  - STA 连不上，等待 60s 后无条件下切 AP：SSID=ESP32S3-Setup / pass=12345678 / 192.168.4.1
 *  - 进入 AP 后每 30s 再试一次 STA；连上就自动收起 AP（APSTA 并存）
 *  - 断线重连用 esp_wifi 内建 reconnect（带退避），禁止 while(!connected) 死等
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_AP_SSID      "ESP32S3-Setup"
#define WIFI_AP_PASS      "12345678"      /* 任务书规定的固定配网密码（非用户密钥） */
#define WIFI_AP_IP        "192.168.4.1"
#define WIFI_STA_TIMEOUT_MS  60000        /* 60s 内没连上就开 AP */

typedef enum {
    WIFI_MODE_NONE = 0,
    WIFI_MODE_STA_CONNECTING,
    WIFI_MODE_STA_CONNECTED,
    WIFI_MODE_AP_CONFIG,      /**< 已开 AP，同时后台继续尝试 STA */
} wifi_hal_mode_t;

/** 初始化（非阻塞：内部起事件循环与定时器，本函数立即返回） */
esp_err_t wifi_hal_init(void);

/** 当前模式 */
wifi_hal_mode_t wifi_hal_mode(void);

/** 是否 STA 已连上 */
bool wifi_hal_sta_connected(void);

/** 当前 IP 字符串（STA 优先，否则 AP 的 IP），无 IP 时返回 "0.0.0.0" */
const char *wifi_hal_ip_str(void);

/** 当前 RSSI（未连接返回 0） */
int wifi_hal_rssi(void);

/** 当前 SSID（未连接返回 ""） */
const char *wifi_hal_ssid(void);

/**
 * @brief 立即用新凭据重连（POST /api/config 改完 wifi_ssid/pass 后调用）
 * @note  先断开再连；若新凭据无效，60s 后会重新落到 AP。
 */
esp_err_t wifi_hal_reconnect(void);

/** 模式字符串，供 /api/status 用 */
const char *wifi_hal_mode_str(void);

/**
 * @brief 推流期间切换 WiFi 省电模式（吞吐优先 / 省电优先）
 *
 * @param streaming true  = 关闭省电（WIFI_PS_NONE），保证 MJPEG 等持续大流量
 *                  false = 恢复省电（WIFI_PS_MIN_MODEM）
 *
 * ★ 为什么必须切换：WIFI_PS_MIN_MODEM 会让 STA 在 DTIM 之间睡眠；本机 AP 的
 *   beacon interval 实测 102.4ms（很长的值），一次睡眠就是上百毫秒停顿 ——
 *   MJPEG 这种持续大流量会被直接卡到 httpd 的 send 超时（"error in send : 11"）。
 *   只在推流期间关闭省电，非推流时保持省电，避免整机功耗无条件上升。
 */
esp_err_t wifi_hal_set_streaming(bool streaming);

#ifdef __cplusplus
}
#endif
