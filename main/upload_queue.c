/**
 * @file upload_queue.c
 * @brief 上传队列 + 上传任务 + 内存环形事件表
 *
 * 队列存在内部 RAM（项本身很小），但 buffer 模式的 JPEG 放在 PSRAM 里。
 * 队列满时的两种策略：
 *   drop_new    : 丢掉**刚要入队**的这一项（默认，保护已在队列里的历史）
 *   drop_oldest : 丢掉**队首**那一项，然后放入新项
 * 两种策略都会显式释放被丢弃项的内存（避免 PSRAM 泄漏）。
 */

#include "upload_queue.h"
#include "config_store.h"
#include "logger.h"
#include "sd_hal.h"
#include "sleep_mgr.h"    /* ★ 队列空了 -> 本次唤醒的上传工作结束，回睡 */
#include "upload_http.h"
#include "upload_telegram.h"
#include "mqtt_hal.h"
#include "camera_hal.h"
#include "led_hal.h"
#include "wifi_hal.h"
#include "time_sync.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = LOG_T_Q;

/* ------------------------------ 事件表 ------------------------------ */
#define EVENTS_RING   50
static char     s_events[EVENTS_RING][384];
static int      s_ev_head;
static int      s_ev_count;
static char     s_last_error[96] = "none";

/* ------------------------------ 队列 ------------------------------ */
static QueueHandle_t s_q;
static volatile int      s_qlen;
static volatile size_t   s_qbytes;
static volatile uint32_t s_ok_count;
static volatile uint32_t s_fail_count;
static volatile uint32_t s_drop_count;
static volatile uint32_t s_no_channel_count;   /* 所有上传通道都未配置而未被上送 */
static uint32_t          s_next_id = 1;

/* ------------------------------ 工具函数 ------------------------------ */
const char *trig_src_str(trig_src_t s)
{
    switch (s) {
    case TRIG_SRC_PIR:         return "pir";
    case TRIG_SRC_BUTTON:      return "button";
    case TRIG_SRC_REMOTE_HTTP: return "remote_http";
    case TRIG_SRC_REMOTE_MQTT: return "remote_mqtt";
    case TRIG_SRC_TIMER:       return "timer";
    case TRIG_SRC_TELEGRAM:    return "telegram";
    default:                   return "unknown";
    }
}

const char *upload_queue_mode_str(void)
{
    /* 有 SD 就 path 模式；降级后就是 buffer 模式 */
    sd_state_t st = sd_hal_state();
    return (st == SD_OK || st == SD_LOWSPACE) ? "path" : "buffer";
}

static char *strdup_psram(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = malloc(n);      /* 退化到内部 RAM */
    }
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static void meta_to_json(const meta_info_t *m, char *out, size_t out_sz)
{
    snprintf(out, out_sz,
             "{\"device_id\":\"%s\",\"event_id\":%u,\"ts\":%lld,\"time_valid\":%s,"
             "\"boot_ms\":%u,\"trigger\":\"%s\",\"rssi\":%d,\"free_heap\":%u,"
             "\"free_psram\":%u,\"sd_state\":\"%s\",\"flash_used\":%d,"
             "\"jpeg_len\":%u,\"sensor\":\"%s\"}",
             m->device_id, (unsigned)m->event_id, (long long)m->ts,
             m->time_valid ? "true" : "false",
             (unsigned)m->boot_ms, trig_src_str(m->trigger), m->rssi,
             (unsigned)m->free_heap, (unsigned)m->free_psram,
             m->sd_state ? m->sd_state : "?", m->flash_used,
             (unsigned)m->jpeg_len, m->sensor ? m->sensor : "?");
}

static void events_push(const char *json)
{
    strlcpy(s_events[s_ev_head], json, sizeof(s_events[s_ev_head]));
    s_ev_head = (s_ev_head + 1) % EVENTS_RING;
    if (s_ev_count < EVENTS_RING) {
        s_ev_count++;
    }
}

static void item_free(queue_item_t *it)
{
    if (!it) {
        return;
    }
    if (it->buf) {
        free(it->buf);       /* heap_caps_malloc 出来的内存用 free 释放 */
        it->buf = NULL;
    }
    if (it->meta_json) {
        free(it->meta_json);
        it->meta_json = NULL;
    }
}

/* ------------------------------ 队列入队 ------------------------------ */
esp_err_t upload_queue_push(bool has_file, const char *rel_path,
                            uint8_t *buf, size_t len,
                            const meta_info_t *meta, uint32_t *out_id)
{
    config_store_t *cfg = config_store_get();
    uint32_t id = s_next_id++;

    char meta_json[512];
    meta_info_t m = *meta;
    m.event_id = id;
    meta_to_json(&m, meta_json, sizeof(meta_json));

    QueueHandle_t q = s_q;
    if (!q) {
        item_free(&(queue_item_t){ .buf = buf });
        strlcpy(s_last_error, "queue_not_init", sizeof(s_last_error));
        return ESP_ERR_INVALID_STATE;
    }

    int maxlen = cfg->max_queue_len;
    if (maxlen < 2)  { maxlen = 2; }
    if (maxlen > 16) { maxlen = 16; }

    /* --- 队列满：按策略处理 --- */
    if (s_qlen >= maxlen) {
        if (strcmp(cfg->queue_policy, "drop_oldest") == 0) {
            queue_item_t old;
            if (xQueueReceive(q, &old, 0) == pdTRUE) {
                s_qlen--;
                s_qbytes -= old.len;
                LOGW(TAG, "full policy=drop_oldest drop id=%u", (unsigned)old.event_id);
                item_free(&old);
            }
        } else {
            LOGE(TAG, "full policy=drop_new id=%u", (unsigned)id);
            s_drop_count++;
            /* ★ 入队失败也不能泄漏：调用方交出的 buf 由这里释放 */
            queue_item_t tmp = { .buf = buf };
            item_free(&tmp);
            strlcpy(s_last_error, "queue_full_drop_new", sizeof(s_last_error));
            return ESP_ERR_NO_MEM;
        }
    }

    queue_item_t it = {0};
    it.event_id = id;
    it.has_file = has_file;
    it.len = len;
    it.attempts = 0;
    if (has_file) {
        strlcpy(it.rel_path, rel_path ? rel_path : "", sizeof(it.rel_path));
    } else {
        it.buf = buf;
    }
    it.meta_json = strdup_psram(meta_json);
    if (!it.meta_json) {
        item_free(&it);
        strlcpy(s_last_error, "meta_alloc_fail", sizeof(s_last_error));
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(q, &it, pdMS_TO_TICKS(50)) != pdTRUE) {
        item_free(&it);
        strlcpy(s_last_error, "queue_send_timeout", sizeof(s_last_error));
        return ESP_ERR_TIMEOUT;
    }
    s_qlen++;
    s_qbytes += len;

    events_push(meta_json);

    if (has_file) {
        LOGI(TAG, "enqueue id=%u mode=path len=%d/%d", (unsigned)id, s_qlen, maxlen);
    } else {
        LOGI(TAG, "enqueue id=%u mode=buffer len=%d/%d bytes=%u",
             (unsigned)id, s_qlen, maxlen, (unsigned)len);
    }

    if (out_id) {
        *out_id = id;
    }
    return ESP_OK;
}

/* ------------------------------ 上传任务 ------------------------------ */
static void handle_one(queue_item_t *it)
{
    config_store_t *cfg = config_store_get();
    bool ok = false;
    bool attempted = false;     /* 是否真的发起过至少一次上传尝试 */
    char err[96] = "none";

    /* 决定这次用哪几种通道 */
    const char *um = cfg->upload_mode;
    bool do_http = (strcmp(um, "http") == 0) || (strcmp(um, "both") == 0);
    bool do_tg   = (strcmp(um, "telegram") == 0) || (strcmp(um, "both") == 0);

    /* ---- HTTP ---- */
    if (do_http && strlen(cfg->http_url) > 0) {
        attempted = true;
        esp_err_t e = upload_http_send(it->meta_json, it->has_file ? NULL : it->buf,
                                       it->len, it->has_file ? it->rel_path : NULL);
        if (e == ESP_OK) {
            ok = true;
        } else {
            strlcpy(err, upload_http_last_error(), sizeof(err));
            LOGW(TAG, "http fail id=%u err=%s attempts=%u",
                 (unsigned)it->event_id, err, (unsigned)it->attempts);
        }
    } else if (do_http) {
        LOGW(TAG, "http skipped (http_url 未配置) id=%u", (unsigned)it->event_id);
    }

    /* ---- Telegram ---- */
    if (do_tg && strlen(cfg->telegram_token) > 0 && strlen(cfg->telegram_chat_id) > 0) {
        attempted = true;
        esp_err_t e = upload_telegram_send_photo(it->meta_json,
                                                 it->has_file ? NULL : it->buf,
                                                 it->len,
                                                 it->has_file ? it->rel_path : NULL);
        if (e == ESP_OK) {
            ok = true;
        } else {
            strlcpy(err, upload_telegram_last_error(), sizeof(err));
            LOGW(TAG, "telegram fail id=%u err=%s", (unsigned)it->event_id, err);
        }
    } else if (do_tg) {
        LOGW(TAG, "telegram skipped (token/chat_id 未配置) id=%u", (unsigned)it->event_id);
    }

    /* ---- MQTT：只发 meta，失败不影响 ok ---- */
#if CONFIG_SNAP_ENABLE_MQTT
    /* ★ 判据必须与 HTTP / Telegram 分支一致：**先确认已配置**才置 attempted。
     *   早期写法无条件 attempted = true，于是"编译期开了 MQTT 但没填 mqtt_uri"时，
     *   每次拍照都会走到下面的 fail 分支：累加 s_fail_count 并打出一条
     *   "upload fail err=none" 的假错误（err 是 none 却报失败，现场极难排查）。 */
    if (cfg->mqtt_uri[0] != '\0') {
        mqtt_hal_publish_meta(it->meta_json);
        attempted = true;   /* 已配置的出口；连不上属真失败，交给下面计数 */
    }
#endif

    if (ok) {
        s_ok_count++;
        strlcpy(s_last_error, "none", sizeof(s_last_error));
        LOGI(TAG, "upload ok id=%u ok=%u fail=%u",
             (unsigned)it->event_id, (unsigned)s_ok_count, (unsigned)s_fail_count);
        led_hal_set_state(LED_ST_UPLOAD_OK);
        /* 上传成功后通知 SD：抬高"可清理水位" */
        if (it->has_file && it->rel_path[0]) {
            sd_hal_note_uploaded_ok(it->rel_path);
        }
        sd_hal_check_space();      /* 顺手做一次低空间检查 */
        return;
    }

    /* ★ 关键区分：所有通道都"未配置" != 上传"失败"。
     *   前者是部署配置缺失（照片留在 SD / PSRAM 并未损坏），把它计成 fail
     *   会让 total_fail 无限累加、并打出一条 err=none 的假错误日志，
     *   严重误导现场排查。这里单独计数、单独措辞。 */
    if (!attempted) {
        s_no_channel_count++;
        LOGW(TAG, "no channel configured id=%u -> 已保留(SD)/丢弃(内存) 本次事件未上送",
             (unsigned)it->event_id);
        return;
    }

    s_fail_count++;
    strlcpy(s_last_error, err, sizeof(s_last_error));
    LOGE(TAG, "upload fail id=%u err=%s total_ok=%u total_fail=%u (已丢弃本次事件)",
         (unsigned)it->event_id, err, (unsigned)s_ok_count, (unsigned)s_fail_count);
    /* 任务书没要求无限重试本地缓存；http 内部已重试 3 次。
     * 这里不再把项塞回队列 —— 否则断网时会无限增长直至 OOM。 */
}

static void upload_task(void *arg)
{
    (void)arg;
    queue_item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_qlen--;
        if (s_qbytes >= it.len) {
            s_qbytes -= it.len;
        } else {
            s_qbytes = 0;
        }

        handle_one(&it);
        item_free(&it);      /* ★ 唯一释放点 */

        /* ★ 2026-09-28 深睡形态：**队列已空 = 本次唤醒的上传工作全部结束**，
         *   立刻回睡（需求原文："上传后就立即深度休眠"）。
         *
         * 判据用 "队列空" 而不是 "这一张传成功"，两个理由：
         *   · 失败也算"做完了"（handle_one 明确不重试、不回队），
         *     否则一张传不出去的照片会让设备一直醒着把电耗光；
         *   · 连拍时队列里可能还有后面的项，必须等它们都处理完。
         *
         * ★ 必须放在 item_free() 之后：醒着时不能留未释放的 PSRAM 副本。
         * ★ sleep_mgr 里还有一层"硬兜底"（sleep_max_awake_ms）覆盖卡死场景。 */
        if (s_qlen == 0) {
            sleep_mgr_notify_idle();
        }
    }
}

/* ------------------------------ init ------------------------------ */
esp_err_t upload_queue_init(void)
{
    config_store_t *cfg = config_store_get();
    int maxlen = cfg->max_queue_len;
    if (maxlen < 2)  { maxlen = 2; }
    if (maxlen > 16) { maxlen = 16; }

    s_q = xQueueCreate(maxlen, sizeof(queue_item_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    s_qlen = 0;
    s_qbytes = 0;

    if (xTaskCreate(upload_task, "uploader", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    LOGI(TAG, "init ok max=%d policy=%s backend=%s",
         maxlen, cfg->queue_policy, upload_queue_mode_str());
    return ESP_OK;
}

/* ------------------------------ 查询 ------------------------------ */
int upload_queue_len(void)
{
    return s_qlen;
}

size_t upload_queue_bytes(void)
{
    return s_qbytes;
}

uint32_t upload_queue_ok_count(void)
{
    return s_ok_count;
}

uint32_t upload_queue_fail_count(void)
{
    return s_fail_count;
}

uint32_t upload_queue_drop_count(void)
{
    return s_drop_count;
}

const char *upload_queue_last_error(void)
{
    return s_last_error;
}

uint32_t upload_queue_no_channel_count(void)
{
    return s_no_channel_count;
}

size_t upload_queue_events_json(char *buf, size_t buf_sz)
{
    size_t o = 0;
    buf[o++] = '[';
    int n = s_ev_count;
    int start = (s_ev_count < EVENTS_RING) ? 0 : s_ev_head;
    for (int i = 0; i < n; i++) {
        const char *e = s_events[(start + i) % EVENTS_RING];
        size_t need = strlen(e) + 2;
        if (o + need >= buf_sz) {
            break;
        }
        if (i) {
            buf[o++] = ',';
        }
        memcpy(buf + o, e, strlen(e));
        o += strlen(e);
    }
    if (o + 2 >= buf_sz) {
        o = buf_sz - 2;
    }
    buf[o++] = ']';
    buf[o] = '\0';
    return o;
}

void upload_queue_kick(void)
{
    /* 上传任务本身就是 portMAX_DELAY 等队列，入队即唤醒。
     * 这个接口保留给"手动催一下"的场景（例如配置改完后）。 */
    LOGI(TAG, "kick (queue len=%d)", s_qlen);
}
