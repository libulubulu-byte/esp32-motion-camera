/**
 * @file web_server.c
 * @brief 内置 web server + mDNS 实现（任务书 5.7）
 *
 * 设计约束（逐条对应任务书）：
 *  - **禁止在 httpd handler 里做阻塞业务**：拍照（含 flash_before_ms + 写卡）是秒级操作，
 *    所有 handler 只做"投递 + 立刻返回"；只有 last_snapshot / photos 读文件属必要短操作。
 *  - **每个 framebuffer 判空 + 唯一释放路径**：本文件不直接持有 framebuffer，
 *    一律经 snapshot_pipeline / camera_hal_return。
 *  - **禁止硬编码密钥**：admin_key 从 config_store 取，/api/config 与日志都脱敏。
 *  - **所有外部输入做校验**：路径参数走白名单字符检查并禁止 ".."（防目录穿越）。
 *  - **OTA 不在 handler 里跑**：ota_hal_perform_async 建临时任务，立即返回 202。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"

#include "web_server.h"
#include "logger.h"
#include "version.h"
#include "pins_config.h"
#include "config_store.h"
#include "wifi_hal.h"
#include "sd_hal.h"
#include "camera_hal.h"
#include "snapshot_pipeline.h"
#include "human_detect.h"   /* /api/detect_raw 直灌调试端点需要 */
#include "trigger_fsm.h"
#include "upload_queue.h"
#include "mqtt_hal.h"
#include "ota_hal.h"
#include "time_sync.h"
#include "led_hal.h"

static const char *TAG = LOG_T_WEB;

/* 请求体上限：配置 + OTA URL 都是小 JSON，4KB 足够；超过直接 413 */
#define WEB_BODY_MAX      4096
/* /api/log 与 /api/events 的应答缓冲放 PSRAM（内部 RAM 很紧） */
#define WEB_LOG_BUF_SZ    (LOG_RING_LINES * LOG_RING_LINE_SZ + 1024)
#define WEB_EVENT_BUF_SZ  (50 * 512 + 512)

static httpd_handle_t s_server = NULL;

/* ========================================================================== */
/*                                小工具                                       */
/* ========================================================================== */

/** 从 PSRAM 拿一块临时缓冲，拿不到再退化到内部 RAM */
static void *web_alloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

/** 统一错误应答：{"error":"..."}，状态码由调用方给 */
static esp_err_t send_error(httpd_req_t *req, const char *msg, const char *status)
{
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, n);
}

static esp_err_t send_ok_json(httpd_req_t *req, const char *json, size_t len)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

/** 序列化 cJSON 后应答（函数负责释放树） */
static esp_err_t send_json_tree(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        return send_error(req, "oom", "500 Internal Server Error");
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return send_error(req, "json serialize failed", "500 Internal Server Error");
    }
    esp_err_t err = send_ok_json(req, txt, strlen(txt));
    cJSON_free(txt);
    return err;
}

/** 取 query 参数；返回 false = 该参数不存在 */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t out_sz)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 256) {
        return false;
    }
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(q, key, out, out_sz) == ESP_OK;
}

/** query 里的布尔参数：1 / t / y 开头都算真 */
static bool query_get_bool(httpd_req_t *req, const char *key, bool def)
{
    char v[16];
    if (!query_get(req, key, v, sizeof(v))) {
        return def;
    }
    return (v[0] == '1') || (v[0] == 't') || (v[0] == 'T') ||
           (v[0] == 'y') || (v[0] == 'Y');
}

/** 读请求体到堆缓冲；超上限返回 NULL（调用方回 413）。调用方 free */
static char *read_body(httpd_req_t *req, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len >= WEB_BODY_MAX) {
        return NULL;
    }
    char *buf = web_alloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;              /* 只重试超时 */
            }
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    if (out_len) {
        *out_len = (size_t)received;
    }
    return buf;
}

/**
 * @brief 校验 X-Admin-Key
 * @retval true 通过（或未设置 admin_key —— 首次配网状态允许改配置）
 */
static bool check_admin(httpd_req_t *req)
{
    const char *expect = config_store_admin_key();
    if (!expect || expect[0] == '\0') {
        return true;
    }
    char got[CFG_MAX_STR];
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Key", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    size_t a = strlen(expect), b = strlen(got);
    if (a != b) {
        return false;
    }
    return memcmp(expect, got, a) == 0;
}

/** 相对路径白名单：只允许 [0-9A-Za-z._/-]，且禁止 ".." 与前导 '/' */
static bool safe_rel_path(const char *p)
{
    if (!p || !*p || p[0] == '/' || strstr(p, "..")) {
        return false;
    }
    for (const char *s = p; *s; s++) {
        char c = *s;
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == '.' || c == '_' ||
                  c == '-' || c == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ========================================================================== */
/*                             GET /api/status                                */
/* ========================================================================== */

static esp_err_t h_status(httpd_req_t *req)
{
    config_store_t *cfg = config_store_get();
    sd_status_t sd = sd_hal_status();

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return send_error(req, "oom", "500 Internal Server Error");
    }

    cJSON_AddStringToObject(o, "device_id", cfg->device_id);
    cJSON_AddStringToObject(o, "fw_version", SNAP_FW_VERSION);
    /* ★ running_partition：本机现在跑的是哪个分区（factory / ota_0 / ota_1）。
     * 为什么需要它：fw_version 是编译期常量，**光看它无法区分"OTA 升上去了"
     * 和"还在跑旧的 factory"** —— 两者的 fw_version 都等于各自镜像的版本，
     * 但只有分区名能证明这次启动到底来自哪块 flash。
     * 配网页把它显示在实时画面卡片顶部，是"OTA 有没有真的生效"的直接判据。 */
    const esp_partition_t *rp = esp_ota_get_running_partition();
    cJSON_AddStringToObject(o, "running_partition", rp ? rp->label : "?");
    cJSON_AddNumberToObject(o, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddBoolToObject(o, "armed", config_store_is_armed());
    cJSON_AddBoolToObject(o, "safe_mode", config_store_is_safe_mode());

    cJSON *wifi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wifi, "mode", wifi_hal_mode_str());
    cJSON_AddStringToObject(wifi, "ip", wifi_hal_ip_str());
    cJSON_AddStringToObject(wifi, "ssid", wifi_hal_ssid());
    cJSON_AddNumberToObject(wifi, "rssi", wifi_hal_rssi());

    cJSON *cam = cJSON_AddObjectToObject(o, "camera");
    cJSON_AddStringToObject(cam, "sensor", camera_hal_sensor_name());
    cJSON_AddStringToObject(cam, "expected_pid", camera_hal_expected_pid_str());
    cJSON_AddNumberToObject(cam, "pid", camera_hal_pid());
    cJSON_AddNumberToObject(cam, "framesize", camera_hal_current_framesize());
    cJSON_AddStringToObject(cam, "framesize_name",
                            camera_hal_framesize_name(camera_hal_current_framesize()));
    /* ★ 报**实际生效值**（来自 camera_hal），不是 /api/config 里的请求值 ——
     *   两者可能不同：请求值被 clamp，或"改大分辨率"要等重启才生效。
     *   0.1.8 之前这里直接报 cfg->jpeg_quality，于是"配置写 q12、传感器实际跑 q14"
     *   这种假象可以一直没人发现。 */
    cJSON_AddNumberToObject(cam, "jpeg_quality", camera_hal_current_quality());

    cJSON *sdj = cJSON_AddObjectToObject(o, "sd");
    cJSON_AddStringToObject(sdj, "state", sd_hal_state_str(sd.state));
    cJSON_AddNumberToObject(sdj, "total_kb", (double)(sd.total_bytes / 1024));
    cJSON_AddNumberToObject(sdj, "free_kb", (double)(sd.free_bytes / 1024));
    cJSON_AddNumberToObject(sdj, "retry_count", sd.retry_count);
    cJSON_AddStringToObject(sdj, "cid", sd.cid);

    cJSON *q = cJSON_AddObjectToObject(o, "queue");
    cJSON_AddNumberToObject(q, "len", upload_queue_len());
    cJSON_AddNumberToObject(q, "bytes", (double)upload_queue_bytes());
    cJSON_AddStringToObject(q, "mode", upload_queue_mode_str());
    cJSON_AddStringToObject(q, "policy", cfg->queue_policy);
    cJSON_AddNumberToObject(q, "ok_count", upload_queue_ok_count());
    cJSON_AddNumberToObject(q, "fail_count", upload_queue_fail_count());
    cJSON_AddNumberToObject(q, "drop_count", upload_queue_drop_count());
    cJSON_AddStringToObject(q, "last_error", upload_queue_last_error());

    cJSON *trig = cJSON_AddObjectToObject(o, "trigger");
    cJSON_AddStringToObject(trig, "state", trig_state_str(trigger_fsm_state()));
    cJSON_AddBoolToObject(trig, "in_cooldown", trigger_fsm_in_cooldown());
    cJSON_AddNumberToObject(trig, "count", trigger_fsm_count());
    cJSON_AddNumberToObject(trig, "cooldown_skips", trigger_fsm_cooldown_skips());
    cJSON_AddNumberToObject(trig, "min_interval_ms", cfg->min_trigger_interval_ms);

    cJSON *snap = cJSON_AddObjectToObject(o, "snapshot");
    cJSON_AddNumberToObject(snap, "ok_count", snapshot_pipeline_ok_count());
    cJSON_AddNumberToObject(snap, "fail_count", snapshot_pipeline_fail_count());
    cJSON_AddNumberToObject(snap, "psram_min", snapshot_pipeline_psram_min());
    /* ★ 人形门控计数：调"是不是人就上传"时，这三个数是唯一能远程看到的东西。
     *   run = 跑过多少次检测；hit = 判为有人；miss = 判为无人（被丢弃）。
     *   run - hit - miss = 走 fail-open 的次数（检测本身出错，照片照常上传）。
     *   若 run 一直是 0，说明检测根本没被调用（开关没开 / 模型没烧进去）。 */
    cJSON_AddNumberToObject(snap, "human_gate_on",
#if CONFIG_SNAP_ENABLE_HUMAN_DETECT
                            1
#else
                            0
#endif
                            );
    cJSON_AddNumberToObject(snap, "human_run",  snapshot_pipeline_human_run_count());
    cJSON_AddNumberToObject(snap, "human_hit",  snapshot_pipeline_human_hit_count());
    cJSON_AddNumberToObject(snap, "human_miss", snapshot_pipeline_human_miss_count());

    cJSON *sys = cJSON_AddObjectToObject(o, "system");
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* ★ 2026-09-28：把**内部 RAM** 单独报出来。
     *
     * 为什么必须有它：free_heap 是"默认堆"的余量，启用 PSRAM 后它把 8MB 外部
     * RAM 也算进去了，于是永远是 7.7MB 的漂亮数字 —— **完全掩盖内部 RAM 快见底**。
     * 而真正会分配失败的恰恰只有内部 RAM：WiFi 驱动、lwIP、以及**每条约 24KB 的
     * TLS 会话缓冲**（IN 16KB + OUT 4KB + SSL 上下文）都只能用它。
     * 现场故障就是
     *     E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00  (ALLOC_FAILED)
     * 排查时却因为 free_heap 看着富余而绕了很久。
     * 有了本字段，这类问题一眼可见（判据：长期低于 ~30KB 就危险）。 */
    cJSON_AddNumberToObject(sys, "heap_internal",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddBoolToObject(sys, "time_valid", time_sync_valid());
    cJSON_AddNumberToObject(sys, "time", (double)time(NULL));
    cJSON_AddStringToObject(sys, "tz", time_sync_tz());
    cJSON_AddStringToObject(sys, "upload_mode", config_store_upload_mode_str());
    cJSON_AddStringToObject(sys, "mqtt", mqtt_hal_state_str());
    cJSON_AddStringToObject(sys, "led", led_hal_get_state() == LED_ST_FATAL ? "fatal" : "normal");
    cJSON_AddBoolToObject(sys, "ota_in_progress", ota_hal_in_progress());
    cJSON_AddStringToObject(sys, "mdns", WEB_MDNS_HOST ".local");

    return send_json_tree(req, o);
}

/* ========================================================================== */
/*                          POST /api/snapshot                                */
/* ========================================================================== */

static esp_err_t h_snapshot_post(httpd_req_t *req)
{
    if (config_store_is_safe_mode()) {
        return send_error(req, "safe mode: camera pipeline disabled",
                          "503 Service Unavailable");
    }
    if (!config_store_is_armed()) {
        return send_error(req, "disarmed", "409 Conflict");
    }

    /* force=1：连拍验收用（绕过冷却由 trigger_fsm 的 min_interval 控制，
     * 这里只作为日志/语义标注；真正的冷却门在 FSM 里，仍然生效） */
    bool force = query_get_bool(req, "force", false);

    esp_err_t err = trigger_fsm_fire(TRIG_SRC_REMOTE_HTTP, force ? "http-force" : "http");
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "cooldown", "409 Conflict");
    }
    if (err != ESP_OK) {
        return send_error(req, "busy", "409 Conflict");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", true);
    cJSON_AddBoolToObject(o, "force", force);
    return send_json_tree(req, o);
}

/* ========================================================================== */
/*                        GET /api/last_snapshot                              */
/* ========================================================================== */

static esp_err_t h_last_snapshot(httpd_req_t *req)
{
    char rel[200] = {0};
    size_t len = 0;
    const uint8_t *mem = snapshot_pipeline_last(rel, sizeof(rel), &len);

    /* 优先 SD：直接把文件读出来流走 */
    if (rel[0] && sd_hal_state() == SD_OK) {
        long sz = sd_hal_file_size(rel);
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            uint8_t *buf = web_alloc((size_t)sz);
            if (buf) {
                int n = sd_hal_read_file(rel, buf, (size_t)sz);
                if (n > 0) {
                    httpd_resp_set_type(req, "image/jpeg");
                    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
                    esp_err_t e = httpd_resp_send(req, (const char *)buf, n);
                    free(buf);
                    return e;
                }
                free(buf);
            }
        }
    }

    /* 降级：内存里最近一张副本（snapshot_pipeline 持有，不要 free） */
    if (mem && len > 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)mem, len);
    }

    return send_error(req, "no snapshot yet", "404 Not Found");
}

/* ========================================================================== */
/*                     POST /api/detect_raw  ★ 直灌调试端点                    */
/* ========================================================================== */

/**
 * @brief 把请求体（一张 JPEG 原图）**直接喂给人脸检测**，返回检测结果 JSON
 *
 * ★ 这个端点只为一件事存在：**把「相机/光学」与「算法链路」彻底隔离。**
 *
 *   真实路径   OV3660 -> JPEG -> sw_decode_jpeg -> ImagePreprocessor -> MSR/MNP
 *   直灌路径   PC 图片 -> (同一份 sw_decode_jpeg) -> (同一个 detector) -> MSR/MNP
 *   两条路在"光 -> 电"之后**走的是完全同一段代码**（见 human_detect_run_raw）。
 *
 *   所以它的结论是二值且硬的：
 *     · 灌一张已知含脸的图**能检出** ⇒ 模型/量化/预处理全都对，
 *       现场检不出就是**相机或拍摄条件**的问题（镜头脏、焦距、光照、距离）；
 *     · 灌进去**也检不出** ⇒ 问题在算法链路本身，与相机无关，
 *       再花时间去调镜头、补光、换站位都是白费。
 *
 * 用法（PC 侧）：
 *     curl --data-binary @face.jpg http://<设备IP>/api/detect_raw
 *
 * ⚠️ 两个实现约束，别踩：
 *   1. **必须用 .jpg 原文件字节，不要 multipart 包装** —— 这里直接把整个
 *      body 当 JPEG 交给解码器，multipart 的边界串会让解码器解不出来。
 *   2. 请求体上限是 WEB_BODY_MAX(4KB)，装不下一张照片，所以这里**单独**
 *      按 content_len 收，不用 read_body()。上限另设 256KB（VGA JPEG 的 3~5 倍，
 *      足够含余量，同时避免被恶意大包打爆 PSRAM）。
 */
static esp_err_t h_detect_raw(httpd_req_t *req)
{
    /* 检测功能没编译进来时直接拒绝，不要静默返回"有人"骗自己 */
#if !CONFIG_SNAP_ENABLE_HUMAN_DETECT
    return send_error(req, "human detect not compiled in", "501 Not Implemented");
#else
    const size_t cap = 256 * 1024;
    if (req->content_len == 0) {
        return send_error(req, "empty body: POST a raw JPEG file", "400 Bad Request");
    }
    if (req->content_len > cap) {
        return send_error(req, "body too large (max 256KB)", "413 Payload Too Large");
    }

    uint8_t *buf = web_alloc(req->content_len);
    if (!buf) {
        return send_error(req, "oom", "500 Internal Server Error");
    }

    /* 收包：与 read_body() 同款循环，只重试超时 */
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, (char *)buf + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return send_error(req, "recv failed", "500 Internal Server Error");
        }
        received += r;
    }

    /* ★ 关键：调用**与相机路径同一个**检测入口（内部走同一份解码+推理代码）。
     *   它会自己打印 PROBE/人脸检测[fake] 等日志，所以排查时同时看 /api/log。 */
    human_detect_result_t res = {};
    bool human = human_detect_run_raw(buf, (size_t)received, &res);
    free(buf);

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return send_error(req, "oom", "500 Internal Server Error");
    }
    cJSON_AddBoolToObject(o, "human", human);
    cJSON_AddNumberToObject(o, "faces", res.num_faces);
    cJSON_AddNumberToObject(o, "score", res.score);
    cJSON_AddNumberToObject(o, "decode_ms", res.decode_ms);
    cJSON_AddNumberToObject(o, "infer_ms", res.infer_ms);
    cJSON_AddNumberToObject(o, "bytes", received);
    cJSON_AddStringToObject(o, "note", "inject path: same decode+inference as camera");
    return send_json_tree(req, o);
#endif
}

/* ========================================================================== */
/*                            GET /api/photos                                 */
/* ========================================================================== */

static esp_err_t h_photos(httpd_req_t *req)
{
    char date[32] = {0};
    if (!query_get(req, "date", date, sizeof(date))) {
        if (time_sync_valid()) {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            strftime(date, sizeof(date), "%Y/%m/%d", &tm_now);
        }
        /* 时间不可信时留空 = 只返回状态，不猜日期 */
    }
    if (date[0] && !safe_rel_path(date)) {
        return send_error(req, "invalid date", "400 Bad Request");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "files");
    if (!arr) {
        cJSON_Delete(o);
        return send_error(req, "oom", "500 Internal Server Error");
    }
    cJSON_AddStringToObject(o, "date", date);
    cJSON_AddStringToObject(o, "state", sd_hal_state_str(sd_hal_state()));

    if (sd_hal_state() == SD_OK && date[0]) {
        /* SD 上文件名粒度是毫秒，穷举不现实；这里按 5 秒粒度 + 前 30 个存在的文件
         * 返回可用的"浏览"列表（完整导出请把卡插到 PC 或用 /api/last_snapshot 逐张取） */
        char rel[200];
        char meta[200];
        for (int hh = 0; hh < 24 && cJSON_GetArraySize(arr) < 30; hh++) {
            for (int mm = 0; mm < 60 && cJSON_GetArraySize(arr) < 30; mm++) {
                for (int ss = 0; ss < 60 && cJSON_GetArraySize(arr) < 30; ss += 5) {
                    snprintf(rel, sizeof(rel), "snapshots/%s/%02d%02d%02d_000.jpg",
                             date, hh, mm, ss);
                    if (!sd_hal_file_exists(rel)) {
                        continue;
                    }
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "path", rel);
                    cJSON_AddNumberToObject(item, "size", sd_hal_file_size(rel));
                    snprintf(meta, sizeof(meta), "snapshots/%s/%02d%02d%02d_000.meta.json",
                             date, hh, mm, ss);
                    cJSON_AddBoolToObject(item, "meta", sd_hal_file_exists(meta));
                    cJSON_AddItemToArray(arr, item);
                }
            }
        }
    }

    cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(arr));
    return send_json_tree(req, o);
}

/* ========================================================================== */
/*                            GET /api/photo                                  */
/* ========================================================================== */

/**
 * 按文件名取单张照片：GET /api/photo?path=snapshots/2026/09/26/192001_012.jpg
 *
 * 与 /api/photos 的分工：
 *   · /api/photos 是"浏览"，按 5 秒粒度穷举，只能看到文件名规整的那部分；
 *   · /api/photo  是"精确取一张"，path 由调用方给出（例如从 .meta.json 或
 *     事件记录里拿到的真实文件名），因此毫秒粒度的文件也能取到。
 *
 * 安全：path 必须通过 safe_rel_path（拒绝绝对路径与 ".."），并且强制限定在
 *       snapshots/ 前缀下 —— 否则可以读到 littlefs 或别处的任意文件。
 */
static esp_err_t h_photo_get(httpd_req_t *req)
{
    char path[200] = {0};
    if (!query_get(req, "path", path, sizeof(path))) {
        return send_error(req, "missing path", "400 Bad Request");
    }
    if (!safe_rel_path(path) || strncmp(path, "snapshots/", 10) != 0) {
        return send_error(req, "invalid path", "400 Bad Request");
    }
    if (sd_hal_state() != SD_OK) {
        return send_error(req, "sd not ready", "503 Service Unavailable");
    }

    long sz = sd_hal_file_size(path);
    if (sz <= 0) {
        return send_error(req, "not found", "404 Not Found");
    }
    if (sz >= 4 * 1024 * 1024) {
        return send_error(req, "file too large", "413 Payload Too Large");
    }

    uint8_t *buf = web_alloc((size_t)sz);
    if (!buf) {
        return send_error(req, "oom", "500 Internal Server Error");
    }
    int n = sd_hal_read_file(path, buf, (size_t)sz);
    if (n <= 0) {
        free(buf);
        return send_error(req, "read failed", "500 Internal Server Error");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, (const char *)buf, n);
    free(buf);
    return e;
}

/* ========================================================================== */
/*                       GET /api/events、/api/log                            */
/* ========================================================================== */

static esp_err_t h_events(httpd_req_t *req)
{
    char *buf = web_alloc(WEB_EVENT_BUF_SZ);
    if (!buf) {
        return send_error(req, "oom", "500 Internal Server Error");
    }
    size_t n = upload_queue_events_json(buf, WEB_EVENT_BUF_SZ);
    esp_err_t err = send_ok_json(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t h_log(httpd_req_t *req)
{
    char *buf = web_alloc(WEB_LOG_BUF_SZ);
    if (!buf) {
        return send_error(req, "oom", "500 Internal Server Error");
    }
    size_t n = logger_dump_json(buf, WEB_LOG_BUF_SZ);
    esp_err_t err = send_ok_json(req, buf, n);
    free(buf);
    return err;
}

/* ========================================================================== */
/*                         配置读写 / arm / disarm                            */
/* ========================================================================== */

static esp_err_t h_config_get(httpd_req_t *req)
{
    return send_json_tree(req, config_store_to_json(true));   /* masked = 密钥打码 */
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    if (!check_admin(req)) {
        return send_error(req, "unauthorized", "401 Unauthorized");
    }
    size_t len = 0;
    char *body = read_body(req, &len);
    if (!body) {
        return send_error(req, "body empty or too large", "413 Payload Too Large");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_error(req, "invalid json", "400 Bad Request");
    }

    /* 先把 Wi-Fi 凭据抄出来（apply 之后 root 仍有效，但抄出来更清晰） */
    char ssid[CFG_MAX_STR] = {0};
    char pass[CFG_MAX_STR] = {0};
    cJSON *js = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *jp = cJSON_GetObjectItem(root, "wifi_pass");
    if (cJSON_IsString(js)) {
        strlcpy(ssid, js->valuestring, sizeof(ssid));
    }
    if (cJSON_IsString(jp)) {
        strlcpy(pass, jp->valuestring, sizeof(pass));
    }

    esp_err_t err = config_store_apply_json(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, "apply failed", "400 Bad Request");
    }

    /* ★ 摄像头参数改完**立即生效**（0.1.8 起）。在那之前 frame_size / jpeg_quality
     *   只是被写进 /config.json，**从来没进过传感器** —— 网页上把分辨率从 VGA 改成
     *   QVGA，抓拍耗时与体积纹丝不动，而界面显示"已保存"，属于最难查的一类"配置骗人"。
     *   注意：这里只接受"改小或同尺寸"，改大要重启（framebuffer 按 init 尺寸分配），
     *   camera_hal_apply_runtime() 会把那种情况拦下并打警告。 */
    {
        config_store_t *c = config_store_get();
        (void)camera_hal_apply_runtime(c->frame_size, c->jpeg_quality);
    }

    if (ssid[0] && pass[0]) {
        LOGI(TAG, "wifi credentials updated -> reconnect");
        wifi_hal_reconnect();
    } else if (ssid[0] != pass[0]) {
        LOGW(TAG, "wifi credentials incomplete (ssid=%s pass=%s) -> keep current",
             ssid[0] ? "set" : "missing", pass[0] ? "set" : "missing");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "saved", true);
    cJSON_AddItemToObject(o, "config", config_store_to_json(true));
    return send_json_tree(req, o);
}

static esp_err_t h_arm_post(httpd_req_t *req)
{
    trigger_fsm_set_armed(true);
    config_store_set_armed(true);
    LOGI(TAG, "armed (ip=%s)", wifi_hal_ip_str());
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "armed", true);
    return send_json_tree(req, o);
}

static esp_err_t h_disarm_post(httpd_req_t *req)
{
    trigger_fsm_set_armed(false);
    config_store_set_armed(false);
    LOGI(TAG, "disarmed");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "armed", false);
    return send_json_tree(req, o);
}

/* ========================================================================== */
/*                                 SD                                         */
/* ========================================================================== */

static esp_err_t h_sd_get(httpd_req_t *req)
{
    sd_status_t sd = sd_hal_status();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", sd_hal_state_str(sd.state));
    cJSON_AddStringToObject(o, "mode", !sd_hal_is_enabled() ? "none"
                             : (sd_hal_is_required() ? "required" : "optional"));
    cJSON_AddNumberToObject(o, "total_kb", (double)(sd.total_bytes / 1024));
    cJSON_AddNumberToObject(o, "free_kb", (double)(sd.free_bytes / 1024));
    cJSON_AddNumberToObject(o, "retry_count", sd.retry_count);
    cJSON_AddNumberToObject(o, "last_error", sd.last_error);
    cJSON_AddStringToObject(o, "cid", sd.cid);
    return send_json_tree(req, o);
}

static esp_err_t h_sd_remount(httpd_req_t *req)
{
    if (!check_admin(req)) {
        return send_error(req, "unauthorized", "401 Unauthorized");
    }
    esp_err_t err = sd_hal_remount();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(o, "err", err);
    cJSON_AddStringToObject(o, "state", sd_hal_state_str(sd_hal_state()));
    return send_json_tree(req, o);
}

/* ========================================================================== */
/*                          POST /api/ota、reboot                             */
/* ========================================================================== */

static esp_err_t h_ota_post(httpd_req_t *req)
{
#if !CONFIG_SNAP_ENABLE_OTA
    return send_error(req, "ota disabled in this build", "501 Not Implemented");
#else
    if (!check_admin(req)) {
        return send_error(req, "unauthorized", "401 Unauthorized");
    }
    if (ota_hal_in_progress()) {
        return send_error(req, "ota already in progress", "409 Conflict");
    }

    char url[CFG_MAX_URL] = {0};
    char md5[40] = {0};

    /* url/md5 两种给法：query（curl 一行）或 JSON body（脚本友好） */
    if (!query_get(req, "url", url, sizeof(url))) {
        size_t len = 0;
        char *body = read_body(req, &len);
        if (body) {
            cJSON *root = cJSON_Parse(body);
            free(body);
            if (root) {
                cJSON *ju = cJSON_GetObjectItem(root, "url");
                cJSON *jm = cJSON_GetObjectItem(root, "md5");
                if (cJSON_IsString(ju)) {
                    strlcpy(url, ju->valuestring, sizeof(url));
                }
                if (cJSON_IsString(jm)) {
                    strlcpy(md5, jm->valuestring, sizeof(md5));
                }
                cJSON_Delete(root);
            }
        }
    } else {
        query_get(req, "md5", md5, sizeof(md5));
    }

    if (url[0] == '\0') {
        return send_error(req, "missing url", "400 Bad Request");
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return send_error(req, "url must be http(s)", "400 Bad Request");
    }

    /* 落盘保存，便于失败后排查（URL 本身不是密钥，日志只打 host 部分） */
    strlcpy(config_store_get()->ota_url, url, CFG_MAX_URL);
    if (md5[0]) {
        strlcpy(config_store_get()->ota_md5, md5, sizeof(config_store_get()->ota_md5));
    }
    config_store_save();

    /* OTA 前必须停 stream（本工程默认不开，接口留好） */
    web_server_stream_stop();

    esp_err_t err = ota_hal_perform_async(url, md5[0] ? md5 : NULL);
    if (err != ESP_OK) {
        return send_error(req, "ota task create failed", "500 Internal Server Error");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", true);
    cJSON_AddStringToObject(o, "url", url);
    cJSON_AddBoolToObject(o, "md5", md5[0] != '\0');
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_tree(req, o);
#endif
}

static esp_err_t h_reboot_post(httpd_req_t *req)
{
    if (!check_admin(req)) {
        return send_error(req, "unauthorized", "401 Unauthorized");
    }
    LOGW(TAG, "reboot requested");
    send_ok_json(req, "{\"rebooting\":true}", 18);
    vTaskDelay(pdMS_TO_TICKS(300));    /* 让应答先发出去 */
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    if (!check_admin(req)) {
        return send_error(req, "unauthorized", "401 Unauthorized");
    }
    LOGW(TAG, "factory reset requested");
    esp_err_t err = trigger_fsm_factory_reset();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "note", "rebooting");
    send_json_tree(req, o);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ========================================================================== */
/*                             MJPEG stream                                   */
/* ========================================================================== */

#if CONFIG_SNAP_ENABLE_STREAM

static volatile bool s_stream_running = false;

#define STREAM_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;
static const char *STREAM_BOUND = "\r\n--" STREAM_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/**
 * 目标帧率。★ 必须限速，不能"能推多快推多快"：
 *   VGA + jpeg_quality 单帧约 8~30KB（取决于场景细节），相机可给 10~20fps
 *   ⇒ 最高数百 KB/s。TCP 发送窗口倒是已经从 5760 提到 16384
 *   （见 sdkconfig 的 CONFIG_LWIP_TCP_SND_BUF_DEFAULT / _WND_DEFAULT），
 *   但持续满速推送仍会让慢客户端（浏览器起播、WiFi 抖动）的发送缓冲填满，
 *   httpd 在 send_wait_timeout(8s) 后报
 *   "httpd_sock_err: error in send : 11"（errno EAGAIN）并断开连接。
 *   限速后速率可控，慢客户端也不会被踢。
 */
#define STREAM_FPS      10

/* ★★ 为什么这里必须用 async handler（踩过大坑，改之前请读完）★★
 *
 * esp_http_server 在 IDF 5.5 里是**单线程**模型：整个组件只有一个
 * httpd 任务，它在 httpd_server() 的 select 循环里 **内联执行所有同步
 * URI handler**（见 httpd_main.c：httpd_process_session → httpd_sess_process）。
 *
 * 所以一个"不返回"的同步 handler 会把整个 HTTP 服务器堵死 ——
 * 不再 accept 新连接、不再服务任何其它请求。现象极好认：
 *     · 设备 ping 得通（WiFi 层正常）
 *     · TCP 三次握手成功（连接在协议栈里排队），但**永远没有响应**
 *     · 设备主动外发（HTTP 上传 / MQTT）不受影响（走的是其它任务）
 *     · 把流停掉，HTTP 立刻恢复（实测 0.31s 返回 200）
 * 这就是"开着实时画面时，网页上其它按钮全部超时"的根本原因。
 *
 * 官方解法（见 esp_http_server.h）：
 *   httpd_req_async_handler_begin() 拿到一份可在别的任务里用的 req，
 *   推流全部在独立任务里做，handler 立刻返回；结束时**必须**调用
 *   httpd_req_async_handler_complete() 归还 socket。
 */
static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char part[64];

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res == ESP_OK) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    while (res == ESP_OK && s_stream_running) {
        int64_t t_frame = esp_timer_get_time() / 1000;

        camera_fb_t *fb = camera_hal_grab(2000);
        if (!fb) {
            LOGW(TAG, "stream: grab timeout -> abort");
            break;
        }
        size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, STREAM_BOUND, strlen(STREAM_BOUND));
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, part, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        /* 唯一释放路径：无论发送成败都归还 */
        camera_hal_return(fb);
        if (res != ESP_OK) {
            break;                     /* 客户端断开 / 发送超时 */
        }

        /* ★ 限速到 STREAM_FPS（原因见 STREAM_FPS 的注释）：
         *   把本帧耗时补足到 1/FPS，保证推送速率不被相机帧率牵着走。
         *   这里用"按帧计时"而不是固定 vTaskDelay，是为了让实际帧率
         *   在发送慢时自然下降，不会累积延迟。 */
        int64_t spent  = esp_timer_get_time() / 1000 - t_frame;
        int64_t target = 1000 / STREAM_FPS;
        if (spent < target) {
            vTaskDelay(pdMS_TO_TICKS(target - spent));
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);   /* 结束 chunked 响应 */
    s_stream_running = false;
    wifi_hal_set_streaming(false);         /* 推流结束 -> 恢复省电 */
    LOGI(TAG, "stream ended");

    /* ★ 必须调用：释放这份 async req 并**归还 socket**。
     *   漏掉它 socket 永不回收，最终 httpd 无法再 accept，
     *   串口会出现 "httpd_accept_conn: error in accept (23)"。 */
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t h_stream(httpd_req_t *req)
{
    if (config_store_is_safe_mode()) {
        return send_error(req, "safe mode", "503 Service Unavailable");
    }
    /* 只允许一个推流客户端：多个客户端会争抢同一个相机，
     * 互相把对方 grab 成 NULL。 */
    if (s_stream_running) {
        return send_error(req, "stream busy (仅支持一个客户端)", "409 Conflict");
    }

    httpd_req_t *ar = NULL;
    esp_err_t e = httpd_req_async_handler_begin(req, &ar);
    if (e != ESP_OK) {
        LOGE(TAG, "stream: async begin fail err=0x%x", e);
        return send_error(req, "async begin failed", "500 Internal Server Error");
    }

    s_stream_running = true;
    wifi_hal_set_streaming(true);          /* 推流期间关闭 WiFi 省电（见 wifi_hal.h） */

    if (xTaskCreate(stream_task, "mjpeg", 4096, ar, 5, NULL) != pdPASS) {
        s_stream_running = false;
        wifi_hal_set_streaming(false);
        httpd_req_async_handler_complete(ar);
        return send_error(req, "stream task create failed", "500 Internal Server Error");
    }

    /* ★ 立刻返回：主任务要继续 accept 新连接、服务其它请求。
     *   推流的实际工作全在 stream_task 里跑。 */
    return ESP_OK;
}

bool web_server_stream_active(void)
{
    return s_stream_running;
}

void web_server_stream_stop(void)
{
    if (s_stream_running) {
        LOGI(TAG, "stop stream (requested)");
        s_stream_running = false;
    }
}

#else  /* !CONFIG_SNAP_ENABLE_STREAM */

bool web_server_stream_active(void) { return false; }
void web_server_stream_stop(void)   {}

#endif

/* ========================================================================== */
/*                     内嵌网页（web/index.html -> 嵌入段）                     */
/* ========================================================================== */

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t h_index(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/** 浏览器会请求 favicon：直接 204，别在串口刷 404 */
static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ========================================================================== */
/*                                注册                                        */
/* ========================================================================== */

static esp_err_t register_handlers(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = h_index },
        { .uri = "/favicon.ico",      .method = HTTP_GET,  .handler = h_favicon },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/snapshot",     .method = HTTP_POST, .handler = h_snapshot_post },
        { .uri = "/api/last_snapshot",.method = HTTP_GET,  .handler = h_last_snapshot },
        { .uri = "/api/detect_raw",   .method = HTTP_POST, .handler = h_detect_raw },
        { .uri = "/api/photos",       .method = HTTP_GET,  .handler = h_photos },
        { .uri = "/api/photo",        .method = HTTP_GET,  .handler = h_photo_get },
        { .uri = "/api/events",       .method = HTTP_GET,  .handler = h_events },
        { .uri = "/api/log",          .method = HTTP_GET,  .handler = h_log },
        { .uri = "/api/config",       .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",       .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/arm",          .method = HTTP_POST, .handler = h_arm_post },
        { .uri = "/api/disarm",       .method = HTTP_POST, .handler = h_disarm_post },
        { .uri = "/api/sd",           .method = HTTP_GET,  .handler = h_sd_get },
        { .uri = "/api/sd/remount",   .method = HTTP_POST, .handler = h_sd_remount },
        { .uri = "/api/ota",          .method = HTTP_POST, .handler = h_ota_post },
        { .uri = "/api/reboot",       .method = HTTP_POST, .handler = h_reboot_post },
        { .uri = "/api/factory_reset",.method = HTTP_POST, .handler = h_factory_reset },
#if CONFIG_SNAP_ENABLE_STREAM
        { .uri = "/api/stream",       .method = HTTP_GET,  .handler = h_stream },
#endif
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(srv, &uris[i]);
        if (err != ESP_OK) {
            LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* ========================================================================== */
/*                                mDNS                                        */
/* ========================================================================== */

static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set(WEB_MDNS_HOST);
    mdns_instance_name_set(SNAP_FW_NAME);

    mdns_txt_item_t txt[] = {
        { "version", SNAP_FW_VERSION },
        { "sensor",  (char *)camera_hal_sensor_name() },
        { "device",  config_store_get()->device_id },
    };
    mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    LOGI(TAG, "mdns: http://%s.local", WEB_MDNS_HOST);
    return ESP_OK;
}

/* ========================================================================== */
/*                                  init                                      */
/* ========================================================================== */

esp_err_t web_server_init(void)
{
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    /* 上传/下载都可能慢，但 handler 里不做长阻塞；socket 超时给 8s 足够 */
    conf.lru_purge_enable    = true;
    conf.max_uri_handlers    = 24;
    /* ★ max_open_sockets 有两个必须同时满足的约束，改大之前先读这里：
     *
     *   ① 上限 = CONFIG_LWIP_MAX_SOCKETS - 3。httpd 内部固定占用 3 个
     *      （1 个 TCP 监听 + 2 个 UDP 控制通道，见 httpd_main.c 的校验：
     *       `if (HTTPD_MAX_SOCKETS < config->max_open_sockets + 3)`）。
     *      ★ 踩过的坑：只把这里改成 12、没动 LWIP_MAX_SOCKETS(=10)，
     *        结果 httpd_start 返回 ESP_ERR_INVALID_ARG，被 main.c 的
     *        ESP_ERROR_CHECK 捕获后 abort —— 设备无限重启。
     *        所以同时把 CONFIG_LWIP_MAX_SOCKETS 提到了 20。
     *
     *   ② 留够给其它组件。LWIP_MAX_SOCKETS 是**全系统**上限，httpd 之外还有
     *      MQTT 客户端、HTTP 上传、mDNS、SNTP 要占。旧的 LWIP=10 意味着
     *      httpd 最多 7、其余全部共享剩下 3 个 —— 开了 MQTT 之后明显偏紧。
     *
     *   为什么要加大：IDF 默认 max_open_sockets=7，其中 3 个被 httpd 内部占用，
     *   留给客户端的只有 4 个。本工程最坏情况是「1 条 MJPEG 流 + 浏览器为页面
     *   持有的多条 keep-alive 连接 + 页面每 5s 的 /api/status 轮询」，
     *   实测会把 4 个占满，症状是：**设备 ping 得通但所有 HTTP 请求超时**；
     *   `Get-NetTCPConnection -RemoteAddress <ip>` 能看到多条连接卡在 FinWait2
     *   （客户端已发 FIN，设备端没及时关闭）。现在 12(可用 9) + LWIP 20，
     *   校验 12+3=15 ≤ 20 通过。 */
    conf.max_open_sockets    = 12;
    conf.recv_wait_timeout   = 8;
    conf.send_wait_timeout   = 8;
    /* TCP keepalive：帮助识别"客户端已经消失、但连接还挂在表里"的情况，
     * 让 socket 尽快回收。对 MJPEG 长连接场景尤其重要 —— 关页面、
     * 切 WiFi 这类情况不会总走干净的四次挥手。 */
    conf.keep_alive_enable   = true;
    conf.keep_alive_idle     = 5;
    conf.keep_alive_interval = 5;
    conf.keep_alive_count    = 3;
    conf.stack_size          = 8192;   /* 本文件里有 cJSON 树，栈给足 */
    conf.uri_match_fn        = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = register_handlers(s_server);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    /* mDNS 失败不算致命：IP 直连仍可用 */
    start_mdns();

    LOGI(TAG, "httpd started port=80 endpoints=ready ip=%s", wifi_hal_ip_str());
    return ESP_OK;
}

