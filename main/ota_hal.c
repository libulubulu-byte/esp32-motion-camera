/**
 * @file ota_hal.c
 * @brief OTA：esp_https_ota + 本地 MD5 校验 + 结果持久化
 *
 * 流程（必须按序，任何一步失败都"不重启、不写分区"）：
 *   1. 取 URL 与期望 MD5（来自请求参数或 config）
 *   2. 调 prepare_cb 停掉 stream 等占用资源的东西
 *   3. 把整个固件先下载到 PSRAM（用 esp_http_client 手工下载，便于边下边算 MD5）
 *   4. 本地算 MD5 与期望值比对 —— **先校验后写分区**，所以错 MD5 绝不会导致
 *      "写坏了然后 bootloop"
 *   5. 校验通过 -> esp_https_ota(begin/write/finish)，写 ota_0，切 boot 分区，重启
 *   6. 结果写 NVS，下次启动由 ota_hal_report_last_result() 打印
 *
 * 为什么不用 esp_https_ota 自带的 image 校验：
 *   它只支持 SHA256 的 image hash（在 header 里），而任务书要求的是"外部给的
 *   MD5"。所以这里把"下载"与"写分区"拆开，MD5 校验放在中间。
 */

#include "ota_hal.h"
#include "config_store.h"
#include "logger.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
/* IDF 的 mbedtls 组件提供 MD5；头文件路径是 mbedtls/md5.h（不是裸 md5.h） */
#include "mbedtls/md5.h"

static const char *TAG = LOG_T_OTA;

#define OTA_NVS_NS      "ota"
#define OTA_NVS_KEY     "last_result"
#define OTA_MAX_IMG     (3 * 1024 * 1024)   /* 分区 0x300000 = 3MB */

static volatile bool s_in_progress;
static char s_last_err[128] = "none";
static ota_result_t s_last = OTA_RES_NONE;

/* ------------------------------------------------------------------ */
/* NVS 结果持久化                                                      */
/* ------------------------------------------------------------------ */
static void save_result(ota_result_t r)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, OTA_NVS_KEY, (uint8_t)r);
    nvs_commit(h);
    nvs_close(h);
}

void ota_hal_report_last_result(void)
{
    nvs_handle_t h;
    uint8_t v = OTA_RES_NONE;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, OTA_NVS_KEY, &v);
        nvs_close(h);
    }
    s_last = (ota_result_t)v;

    /* 任务书 5.9：下次启动打印 [OTA] last_result=ok/fail */
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (v == OTA_RES_OK) {
        LOGI(TAG, "last_result=ok running=%s", run ? run->label : "?");
        /* 标记这次启动是"一次成功升级后的启动"，让 app_update 确认回滚状态 */
        esp_ota_mark_app_valid_cancel_rollback();
    } else if (v == OTA_RES_FAIL) {
        LOGW(TAG, "last_result=fail (详见升级当次日志)");
    } else {
        LOGI(TAG, "last_result=none running=%s", run ? run->label : "?");
    }
}

ota_result_t ota_hal_last_result(void)
{
    return s_last;
}

bool ota_hal_in_progress(void)
{
    return s_in_progress;
}

const char *ota_hal_last_error(void)
{
    return s_last_err;
}

/* ------------------------------------------------------------------ */
/* 下载到 PSRAM + 计算 MD5                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} dl_ctx_t;

static esp_err_t dl_event(esp_http_client_event_t *evt)
{
    dl_ctx_t *c = (dl_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (c->buf && (c->len + evt->data_len) <= c->cap) {
            memcpy(c->buf + c->len, evt->data, evt->data_len);
            c->len += evt->data_len;
        } else {
            /* 超出 cap：记录但不写入（后面会因长度不符而失败） */
            c->len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t download_firmware(const char *url, dl_ctx_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cap = OTA_MAX_IMG;
    out->buf = heap_caps_malloc(out->cap, MALLOC_CAP_SPIRAM);
    if (!out->buf) {
        out->buf = malloc(out->cap);
    }
    if (!out->buf) {
        snprintf(s_last_err, sizeof(s_last_err), "firmware buffer alloc fail (%dKB)",
                 OTA_MAX_IMG / 1024);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cc = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = dl_event,
        .user_data = out,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cc);
    if (!cli) {
        free(out->buf);
        out->buf = NULL;
        snprintf(s_last_err, sizeof(s_last_err), "http client init fail");
        return ESP_FAIL;
    }
    esp_err_t e = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (e != ESP_OK || status != 200) {
        snprintf(s_last_err, sizeof(s_last_err), "download fail http=%d err=0x%x", status, e);
        free(out->buf);
        out->buf = NULL;
        return ESP_FAIL;
    }
    if (out->len == 0 || out->len > out->cap) {
        snprintf(s_last_err, sizeof(s_last_err), "firmware size bad %u", (unsigned)out->len);
        free(out->buf);
        out->buf = NULL;
        return ESP_FAIL;
    }
    /* 合法性快速检查：必须是 ESP 镜像（magic 0xE9） */
    if (out->buf[0] != 0xE9) {
        snprintf(s_last_err, sizeof(s_last_err), "not an esp image (magic 0x%02x)", out->buf[0]);
        free(out->buf);
        out->buf = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void md5_to_hex(const uint8_t d[16], char out[33])
{
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 0x0F];
    }
    out[32] = '\0';
}

/* ------------------------------------------------------------------ */
esp_err_t ota_hal_perform(const char *url, const char *md5_hex, ota_prepare_cb_t prepare_cb)
{
    if (s_in_progress) {
        snprintf(s_last_err, sizeof(s_last_err), "already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || strlen(url) < 8) {
        snprintf(s_last_err, sizeof(s_last_err), "url empty");
        return ESP_ERR_INVALID_ARG;
    }

    s_in_progress = true;
    snprintf(s_last_err, sizeof(s_last_err), "none");
    LOGI(TAG, "start url=%.120s md5=%.32s", url, (md5_hex && md5_hex[0]) ? md5_hex : "(skip)");

    esp_err_t ret = ESP_FAIL;

    /* ---- 1. 停 stream / 释放资源 ---- */
    if (prepare_cb) {
        LOGI(TAG, "prepare callback (stop stream etc.)");
        prepare_cb();
    }

    /* ---- 2. 下载固件 ---- */
    dl_ctx_t dl;
    if (download_firmware(url, &dl) != ESP_OK) {
        LOGE(TAG, "download failed: %s", s_last_err);
        goto fail;
    }
    LOGI(TAG, "downloaded %u bytes (%.2f MB)", (unsigned)dl.len, dl.len / 1048576.0);

    /* ---- 3. MD5 校验（★ 在写分区之前） ---- */
    if (md5_hex && md5_hex[0] && strlen(md5_hex) == 32) {
        uint8_t digest[16];
        mbedtls_md5_context ctx;
        mbedtls_md5_init(&ctx);
        mbedtls_md5_starts(&ctx);
        mbedtls_md5_update(&ctx, dl.buf, dl.len);
        mbedtls_md5_finish(&ctx, digest);
        mbedtls_md5_free(&ctx);

        char got[33];
        md5_to_hex(digest, got);
        if (strcasecmp(got, md5_hex) != 0) {
            snprintf(s_last_err, sizeof(s_last_err), "md5 mismatch got=%s want=%.32s", got, md5_hex);
            LOGE(TAG, "md5 mismatch: got=%s want=%.32s -> 拒绝写入，不重启", got, md5_hex);
            free(dl.buf);
            goto fail;
        }
        LOGI(TAG, "md5 ok %s", got);
    } else {
        LOGW(TAG, "md5 未提供 -> 跳过本地校验（仍会走 image header 校验）");
    }

    /* ---- 4. 写分区 ---- */
    {
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part) {
            snprintf(s_last_err, sizeof(s_last_err), "no ota partition");
            free(dl.buf);
            goto fail;
        }
        if (dl.len > part->size) {
            snprintf(s_last_err, sizeof(s_last_err), "image %uKB > partition %uKB",
                     (unsigned)(dl.len / 1024), (unsigned)(part->size / 1024));
            free(dl.buf);
            goto fail;
        }

        esp_ota_handle_t h = 0;
        esp_err_t e = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h);
        if (e != ESP_OK) {
            snprintf(s_last_err, sizeof(s_last_err), "ota_begin 0x%x", e);
            free(dl.buf);
            goto fail;
        }
        /* 分块写：避免 3MB 单次调用把 OTA 任务的栈/时间片占满 */
        size_t off = 0;
        const size_t chunk = 32 * 1024;
        while (off < dl.len) {
            size_t n = (dl.len - off > chunk) ? chunk : (dl.len - off);
            e = esp_ota_write(h, dl.buf + off, n);
            if (e != ESP_OK) {
                snprintf(s_last_err, sizeof(s_last_err), "ota_write@%u 0x%x", (unsigned)off, e);
                esp_ota_abort(h);
                free(dl.buf);
                goto fail;
            }
            off += n;
        }
        free(dl.buf);
        dl.buf = NULL;

        e = esp_ota_end(h);
        if (e != ESP_OK) {
            /* 0x105 = ESP_ERR_NOT_FOUND：image 校验失败（magic/hash 不符） */
            snprintf(s_last_err, sizeof(s_last_err), "ota_end 0x%x (image invalid)", e);
            goto fail;
        }
        e = esp_ota_set_boot_partition(part);
        if (e != ESP_OK) {
            /* ★ 这里最容易踩的坑：0x105 = ESP_ERR_NOT_FOUND，含义是
             *   "分区表里没有 otadata 分区"，**不是** "找不到 ota 分区"。
             *   前面几行（debug 级）会打 "E esp_ota_ops: not found otadata"，
             *   但那是 IDF 内部的 ESP_LOGE，容易被淹没在刷屏里。
             *   现象极具误导性：下载成功、MD5 通过、ota_begin/ota_write/ota_end
             *   全部返回 OK（镜像确实写进了 ota_0），只有最后这一步失败，
             *   于是 bootloader 仍从 factory 启动，版本号永远不变 ——
             *   看起来像"升不了级"，实际是分区表少了一行 otadata。 */
            if (e == ESP_ERR_NOT_FOUND) {
                snprintf(s_last_err, sizeof(s_last_err),
                         "set_boot_partition 0x%x: 分区表缺少 otadata 分区！"
                         "请在 partitions.csv 的 nvs 之后加一行 "
                         "'otadata, data, ota, 0xf000, 0x2000' 并重新 fullclean 烧录",
                         e);
            } else {
                snprintf(s_last_err, sizeof(s_last_err), "set_boot_partition 0x%x", e);
            }
            goto fail;
        }
        LOGI(TAG, "write ok part=%s -> reboot in 1s to apply v%s", part->label, SNAP_FW_VERSION);
    }

    /* ---- 5. 成功：记录并重启 ---- */
    save_result(OTA_RES_OK);
    s_last = OTA_RES_OK;
    s_in_progress = false;
    ret = ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

fail:
    /* 失败：记录但不重启 —— 这是"不 bootloop"的关键 */
    save_result(OTA_RES_FAIL);
    s_last = OTA_RES_FAIL;
    s_in_progress = false;
    LOGE(TAG, "ota failed: %s (不重启，继续运行 v%s)", s_last_err, SNAP_FW_VERSION);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 异步版本：web handler 调用，避免阻塞 httpd                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char *url;
    char *md5;
} ota_task_arg_t;

static void ota_task(void *arg)
{
    ota_task_arg_t *a = (ota_task_arg_t *)arg;
    ota_hal_perform(a->url, a->md5, NULL);
    free(a->url);
    free(a->md5);
    free(a);
    vTaskDelete(NULL);
}

esp_err_t ota_hal_perform_async(const char *url, const char *md5_hex)
{
    if (!url || strlen(url) < 8) {
        snprintf(s_last_err, sizeof(s_last_err), "url empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_in_progress) {
        snprintf(s_last_err, sizeof(s_last_err), "already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    ota_task_arg_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return ESP_ERR_NO_MEM;
    }
    a->url = strdup(url);
    a->md5 = strdup(md5_hex ? md5_hex : "");
    if (!a->url || !a->md5) {
        free(a->url);
        free(a->md5);
        free(a);
        return ESP_ERR_NO_MEM;
    }
    /* 栈要够大：下载 + 写分区 + 可能的 TLS 握手 */
    if (xTaskCreate(ota_task, "ota", 8192, a, 5, NULL) != pdPASS) {
        free(a->url);
        free(a->md5);
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
