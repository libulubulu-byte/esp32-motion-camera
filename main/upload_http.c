/**
 * @file upload_http.c
 * @brief HTTP 上传：multipart/form-data，带重试与超时
 *
 * 必须遵守（任务书 0.9 / 5.6）：
 *   - 读写超时 10s（connect 也 10s）
 *   - 重试 3 次，退避 1 / 2 / 4 s
 *   - 只有 200 / 201 / 202 算成功
 *   - 绝不死等：所有等待都有超时，失败即返回
 *
 * 内存策略：multipart body 整体拼在 PSRAM 里（JPEG 最大 1.5MB），
 * 拼完一次 post_field 发出，避免 http client 的 chunked 分片复杂度。
 */

#include "upload_http.h"
#include "config_store.h"
#include "logger.h"
#include "sd_hal.h"
#include "led_hal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
/* HTTPS 用 CA bundle 验证证书（不要 .skip_cert_common_name_check / insecure）。
 * 头文件是 esp_crt_bundle.h，组件是 esp-tls（已在 PRIV_REQUIRES）。
 * 注意：需要在 menuconfig 里打开 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE。 */
#include "esp_crt_bundle.h"

static const char *TAG = LOG_T_UPLOAD;

#define HTTP_TIMEOUT_MS     10000
#define HTTP_RETRY_MAX      3
static const uint32_t backoff_ms[HTTP_RETRY_MAX] = {1000, 2000, 4000};

static char s_last_err[96] = "none";

#define SET_ERR(fmt, ...) snprintf(s_last_err, sizeof(s_last_err), fmt, ##__VA_ARGS__)

const char *upload_http_last_error(void)
{
    return s_last_err;
}

/* 把 rel_path 对应的 JPEG 读进 PSRAM。调用方负责 free（用 free()）。
 * 返回 NULL 表示失败。 */
static uint8_t *load_jpeg(const uint8_t *buf, size_t len, const char *rel_path,
                          size_t *out_len)
{
    if (buf && len) {
        uint8_t *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!p) {
            p = malloc(len);
        }
        if (!p) {
            return NULL;
        }
        memcpy(p, buf, len);
        *out_len = len;
        return p;
    }
    if (!rel_path) {
        return NULL;
    }
    long sz = sd_hal_file_size(rel_path);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        SET_ERR("file_size_bad");
        return NULL;
    }
    uint8_t *p = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = malloc((size_t)sz);
    }
    if (!p) {
        SET_ERR("jpeg_alloc_fail");
        return NULL;
    }
    int r = sd_hal_read_file(rel_path, p, (size_t)sz);
    if (r != (int)sz) {
        SET_ERR("file_read_fail");
        free(p);
        return NULL;
    }
    *out_len = (size_t)sz;
    return p;
}

static esp_err_t do_post(const char *url, const char *key, const char *val,
                         const char *meta_json, const uint8_t *jpeg, size_t jpeg_len,
                         int *out_status)
{
    char *body = NULL;
    esp_err_t ret = ESP_FAIL;

    /* ---------- 组装 multipart body ---------- */
    const char *BOUND = "----ESP32S3SnapKitBoundary";
    size_t meta_len = strlen(meta_json);

    /* 预估容量：边界 + meta 头 + meta + 文件头 + 文件 + 结尾 */
    size_t cap = meta_len + jpeg_len + 512;
    body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        SET_ERR("body_alloc_fail");
        return ESP_ERR_NO_MEM;
    }

    size_t o = 0;
    o += snprintf(body + o, cap - o,
                  "--%s\r\n"
                  "Content-Disposition: form-data; name=\"meta\"\r\n"
                  "Content-Type: application/json\r\n\r\n", BOUND);
    memcpy(body + o, meta_json, meta_len);
    o += meta_len;
    o += snprintf(body + o, cap - o,
                  "\r\n--%s\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"snapshot.jpg\"\r\n"
                  "Content-Type: image/jpeg\r\n\r\n", BOUND);
    if (jpeg_len && o + jpeg_len + 64 < cap) {
        memcpy(body + o, jpeg, jpeg_len);
        o += jpeg_len;
    }
    o += snprintf(body + o, cap - o, "\r\n--%s--\r\n", BOUND);

    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", BOUND);

    /* ---------- 发起请求 ---------- */
    esp_http_client_config_t cc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        /* 用户可填 https URL，证书走 mbedtls 内置 bundle */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cc);
    if (!cli) {
        free(body);
        SET_ERR("client_init_fail");
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Content-Type", ctype);
    if (key && val && val[0]) {
        esp_http_client_set_header(cli, key, val);
    }
    esp_http_client_set_post_field(cli, body, (int)o);

    esp_err_t e = esp_http_client_perform(cli);
    if (e == ESP_OK) {
        int status = esp_http_client_get_status_code(cli);
        if (out_status) {
            *out_status = status;
        }
        /* 任务书：仅 200/201/202 成功 */
        ret = (status == 200 || status == 201 || status == 202) ? ESP_OK : ESP_FAIL;
        if (ret != ESP_OK) {
            SET_ERR("http_status_%d", status);
        }
    } else {
        SET_ERR("perform_fail_0x%x", e);
    }
    esp_http_client_cleanup(cli);
    free(body);
    return ret;
}

esp_err_t upload_http_send(const char *meta_json, const uint8_t *buf, size_t len,
                           const char *rel_path)
{
    config_store_t *cfg = config_store_get();
    if (strlen(cfg->http_url) == 0) {
        SET_ERR("url_not_configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (!meta_json) {
        SET_ERR("meta_null");
        return ESP_ERR_INVALID_ARG;
    }

    size_t jpeg_len = 0;
    uint8_t *jpeg = load_jpeg(buf, len, rel_path, &jpeg_len);
    if (!jpeg) {
        if (strcmp(s_last_err, "none") == 0) {
            SET_ERR("jpeg_load_fail");
        }
        return ESP_FAIL;
    }

    /* 只打 host 与长度，不打完整 URL（URL 里可能带 token query） */
    const char *host = strstr(cfg->http_url, "://");
    host = host ? host + 3 : cfg->http_url;
    LOGI(TAG, "http POST host=%.*s len=%u", (int)strcspn(host, "/"), host,
         (unsigned)jpeg_len);

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt <= HTTP_RETRY_MAX; attempt++) {
        if (attempt > 0) {
            uint32_t wait = backoff_ms[attempt - 1];
            LOGW(TAG, "http retry %d/%d after %ums err=%s", attempt, HTTP_RETRY_MAX,
                 (unsigned)wait, s_last_err);
            /* ★ 用可被删除的延时，保持任务响应性；这里没有 abort 条件，
             *   但 4s 上限在可接受范围内，且不会无限等 */
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
        int status = 0;
        ret = do_post(cfg->http_url, cfg->http_header_key, cfg->http_header_value,
                      meta_json, jpeg, jpeg_len, &status);
        if (ret == ESP_OK) {
            LOGI(TAG, "http ok status=%d retry=%d", status, attempt);
            break;
        }
        LOGW(TAG, "http fail attempt=%d err=%s", attempt, s_last_err);
    }

    free(jpeg);
    if (ret != ESP_OK) {
        LOGW(TAG, "http give up after %d retries err=%s", HTTP_RETRY_MAX, s_last_err);
    }
    return ret;
}
