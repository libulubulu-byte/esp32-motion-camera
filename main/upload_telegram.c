/**
 * @file upload_telegram.c
 * @brief Telegram Bot：sendPhoto + 远程命令
 *
 * 安全（任务书 0.8 / 5.6）：
 *   - token 只从 config 读，绝不硬编码
 *   - 任何日志里 token 只打前 6 位 + "***"
 *
 * 重试：与 HTTP 一致，3 次退避 1/2/4s，超时 10s，仅 200 成功。
 * 限流：同一 chat 5s 内只允许一次 sendPhoto。
 */

#include "upload_telegram.h"
#include "config_store.h"
#include "logger.h"
#include "sd_hal.h"
#include "led_hal.h"
#include "wifi_hal.h"
#include "trigger_fsm.h"
#include "camera_hal.h"
#include "upload_queue.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"   /* api.telegram.org 是 HTTPS，必须验证证书 */
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"   /* RTC_DATA_ATTR：跨深睡保留 getUpdates 的 offset */
#include "cJSON.h"

static const char *TAG = LOG_T_TG;

/* ★ 2026-09-28 实测：设备经"PC 侧透明中继 -> 本机代理 -> Telegram"这条链路，
 *   单次 TLS 握手/SendPhoto 会**偶发失败**（真机连续 3 次里失败 2 次、第 3 次成功）。
 *   原来 10s 超时 + 3 次重试太紧，照片常常要等到最后一次才侥幸发出去。
 *   现改为 15s 超时 + 5 次重试（退避 1/2/3/4/5s，总计 15s，最坏 ≈ 90s 封顶）。 */
#define TG_TIMEOUT_MS     15000
#define TG_RETRY_MAX      5
#define TG_THROTTLE_MS    5000
#define TG_API            "https://api.telegram.org"

static char s_last_err[96] = "none";
static int64_t s_last_send_ms;

/* ★★ 必须跨深睡保留（RTC_DATA_ATTR）★★
 *
 * 踩过的坑（2026-09-28 现场实测）：Telegram 的规则是"用 offset = 某条 id + 1
 * 去拉，才会把 ≤ 该 id 的更新**永久删除**"。而本变量原来是普通 static ——
 * 深睡唤醒是**完整重启**，它每次都复位成 0，于是永远用 offset=1 去拉、
 * **永远不确认**。后果：最后收到的那条 /snap 被无限重复投递，设备每醒一次
 * 就当新命令执行一次。现场表现："每次定时唤醒都往 Telegram 传一张图，
 * 而且不判人形"（/snap 是远程命令，本来就免人形门控）。
 *
 * 放 RTC 慢存：深睡不掉电所以能留住；上电才清，而清掉也无害 ——
 * 之前已确认过的更新在服务端早已删除，offset 回到 1 只会拿到真正的新消息。 */
RTC_DATA_ATTR static int64_t s_update_offset;

static char s_bot_name[64];

/* 定时唤醒路径的协作计数：**app_main 不能自己跑轮询**（它的栈只有 8KB，
 * TLS + esp_http_client + cJSON 会爆栈 panic 复位，见 main.c 的
 * SLEEP_WAKE_TIMER 分支），必须让 tg_poll_task 去跑；跑完这里 +1，
 * app_main 等这个计数推进再决定回睡。 */
static volatile int s_poll_done;

#define SET_ERR(fmt, ...) snprintf(s_last_err, sizeof(s_last_err), fmt, ##__VA_ARGS__)

const char *upload_telegram_last_error(void)
{
    return s_last_err;
}

const char *upload_telegram_bot_name(void)
{
    return s_bot_name;
}

/* ---------------------------------------------------------------------- */
/* 通用 POST（form-urlencoded 或 multipart）                              */
/* ---------------------------------------------------------------------- */

/** 简单的 form-urlencoded POST，用于 sendMessage / getUpdates / getMe */
static esp_err_t tg_post_simple(const char *method, const char *body,
                                char *resp, size_t resp_sz, const char *ctype)
{
    config_store_t *cfg = config_store_get();
    if (cfg->telegram_token[0] == '\0') {
        SET_ERR("token_not_configured");
        return ESP_ERR_INVALID_ARG;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/bot%s/%s", TG_API, cfg->telegram_token, method);

    esp_http_client_config_t cc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TG_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cc);
    if (!cli) {
        SET_ERR("client_init_fail");
        return ESP_FAIL;
    }
    esp_http_client_set_header(cli, "Content-Type", ctype ? ctype : "application/x-www-form-urlencoded");

    /* ★★ 必须用「手工三段式」，不能用 esp_http_client_perform() + read_response()。
     *
     * 已从 IDF 源码核实（components/esp_http_client/esp_http_client.c 的
     * esp_http_client_perform() 末尾）：
     *     while (client->response->is_chunked && !client->is_chunk_complete) get_data();
     *     while (client->response->data_process < client->response->content_length) get_data();
     * perform() 会**把响应体整个读掉并丢弃**，之后再调 read_response() 只能拿到 0 字节。
     *
     * 后果（2026-09-27 真机踩到，极难定位）：
     *     · getUpdates 永远拿不到 JSON -> /snap /status 等命令"完全没反应"；
     *     · sendMessage / getMe 同理；
     *     · 而且**完全静默**：SET_ERR 只写错误串不打印，调用方又忽略返回值，
     *       串口一条日志都没有，看着像"网络不通"，实际请求早就成功、只是 body 被丢了。
     * 正确姿势：open(带 body 长度) -> write(body) -> fetch_headers -> read_response。
     * （open() 只发请求头，body 必须自己 write —— 见 esp_http_client.c:1731 的 open 实现
     *   和 1709 的 send_post_data，后者只有 perform() 会调用。） */
    esp_err_t ret = ESP_FAIL;
    int body_len = (int)strlen(body);
    esp_err_t e = esp_http_client_open(cli, body_len);
    if (e == ESP_OK) {
        if (esp_http_client_write(cli, body, body_len) < 0) {
            SET_ERR("write_fail");
        } else {
            esp_http_client_fetch_headers(cli);
            int status = esp_http_client_get_status_code(cli);
            if (resp && resp_sz) {
                int rd = esp_http_client_read_response(cli, resp, (int)resp_sz - 1);
                resp[rd > 0 ? rd : 0] = '\0';
            }
            if (status == 200) {
                ret = ESP_OK;
            } else {
                SET_ERR("status_%d", status);
            }
        }
        esp_http_client_close(cli);
    } else {
        SET_ERR("open_0x%x", e);
    }
    esp_http_client_cleanup(cli);
    return ret;
}

static esp_err_t tg_call_with_retry(const char *method, const char *body,
                                    char *resp, size_t resp_sz, const char *ctype)
{
    static const uint32_t backoff[TG_RETRY_MAX] = {1000, 2000, 3000, 4000, 5000};
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i <= TG_RETRY_MAX; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff[i - 1]));
            LOGW(TAG, "retry %d/%d after %ums err=%s", i, TG_RETRY_MAX,
                 (unsigned)backoff[i - 1], s_last_err);
        }
        ret = tg_post_simple(method, body, resp, resp_sz, ctype);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }
    return ret;
}

/* ---------------------------------------------------------------------- */
/* URL 编码（caption / JSON 里的中文与特殊字符必须转义）                    */
/* ---------------------------------------------------------------------- */
static size_t url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dst_sz; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[o++] = (char)*p;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[*p >> 4];
            dst[o++] = hex[*p & 0x0F];
        }
    }
    dst[o] = '\0';
    return o;
}

/* ---------------------------------------------------------------------- */
/* sendPhoto                                                              */
/* ---------------------------------------------------------------------- */
static uint8_t *tg_load_jpeg(const uint8_t *buf, size_t len, const char *rel_path,
                             size_t *out_len)
{
    if (buf && len) {
        uint8_t *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!p) {
            p = malloc(len);
        }
        if (p) {
            memcpy(p, buf, len);
            *out_len = len;
        }
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
    if (sd_hal_read_file(rel_path, p, (size_t)sz) != (int)sz) {
        SET_ERR("file_read_fail");
        free(p);
        return NULL;
    }
    *out_len = (size_t)sz;
    return p;
}

/** 从 meta JSON 里抽几个字段拼 caption */
static void build_caption(const char *meta_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(meta_json);
    const char *trig = "?";
    int rssi = 0;
    unsigned jlen = 0;
    const char *sdst = "?";
    if (root) {
        const cJSON *t = cJSON_GetObjectItem(root, "trigger");
        const cJSON *r = cJSON_GetObjectItem(root, "rssi");
        const cJSON *j = cJSON_GetObjectItem(root, "jpeg_len");
        const cJSON *s = cJSON_GetObjectItem(root, "sd_state");
        if (cJSON_IsString(t)) { trig = t->valuestring; }
        if (cJSON_IsNumber(r)) { rssi = r->valueint; }
        if (cJSON_IsNumber(j)) { jlen = (unsigned)j->valueint; }
        if (cJSON_IsString(s)) { sdst = s->valuestring; }
    }
    snprintf(out, out_sz, "%s v%s | trig=%s rssi=%d %uKB sd=%s",
             SNAP_FW_NAME, SNAP_FW_VERSION, trig, rssi, (jlen + 512) / 1024, sdst);
    if (root) {
        cJSON_Delete(root);
    }
}

/** multipart 版 sendPhoto（唯一必须用 multipart 的接口） */
static esp_err_t tg_send_photo(const char *chat_id, const char *caption,
                               const uint8_t *jpeg, size_t jpeg_len)
{
    config_store_t *cfg = config_store_get();
    char url[256];
    snprintf(url, sizeof(url), "%s/bot%s/sendPhoto", TG_API, cfg->telegram_token);

    const char *BOUND = "----ESP32S3SnapKitTgBoundary";
    size_t cap = jpeg_len + strlen(caption) * 3 + 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        SET_ERR("body_alloc_fail");
        return ESP_ERR_NO_MEM;
    }

    size_t o = 0;
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                  BOUND, chat_id);
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n"
                  "%.400s\r\n", BOUND, caption);
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"photo\"; "
                  "filename=\"snapshot.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", BOUND);
    if (jpeg_len && o + jpeg_len + 64 < cap) {
        memcpy(body + o, jpeg, jpeg_len);
        o += jpeg_len;
    }
    o += snprintf(body + o, cap - o, "\r\n--%s--\r\n", BOUND);

    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", BOUND);

    esp_http_client_config_t cc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TG_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cc);
    esp_err_t ret = ESP_FAIL;
    if (!cli) {
        free(body);
        SET_ERR("client_init_fail");
        return ESP_FAIL;
    }
    esp_http_client_set_header(cli, "Content-Type", ctype);

    /* 同 tg_post_simple：perform() 会把响应体读空，必须手工三段式（长注释见上） */
    esp_err_t e = esp_http_client_open(cli, (int)o);
    if (e == ESP_OK) {
        if (esp_http_client_write(cli, body, (int)o) < 0) {
            SET_ERR("write_fail");
        } else {
            esp_http_client_fetch_headers(cli);
            int status = esp_http_client_get_status_code(cli);
            char resp[256] = {0};
            int rd = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
            resp[rd > 0 ? rd : 0] = '\0';
            /* Telegram 出错时也返回 200，但 JSON 里 ok=false → 必须看 body */
            if (status == 200 && strstr(resp, "\"ok\":true") != NULL) {
                ret = ESP_OK;
            } else {
                SET_ERR("api_err_%d", status);
                LOGW(TAG, "sendPhoto resp: %.120s", resp);
            }
        }
        esp_http_client_close(cli);
    } else {
        SET_ERR("open_0x%x", e);
    }
    esp_http_client_cleanup(cli);
    free(body);
    return ret;
}

esp_err_t upload_telegram_send_photo(const char *meta_json, const uint8_t *buf,
                                     size_t len, const char *rel_path)
{
    config_store_t *cfg = config_store_get();
    if (cfg->telegram_token[0] == '\0' || cfg->telegram_chat_id[0] == '\0') {
        SET_ERR("not_configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (!meta_json) {
        SET_ERR("meta_null");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 同 chat 5s 节流 ---- */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_send_ms && (now_ms - s_last_send_ms) < TG_THROTTLE_MS) {
        LOGW(TAG, "throttled (chat=%s, %lldms since last, limit=%dms)",
             cfg->telegram_chat_id, (long long)(now_ms - s_last_send_ms), TG_THROTTLE_MS);
        SET_ERR("throttled");
        return ESP_ERR_INVALID_STATE;
    }

    size_t jpeg_len = 0;
    uint8_t *jpeg = tg_load_jpeg(buf, len, rel_path, &jpeg_len);
    if (!jpeg) {
        if (strcmp(s_last_err, "none") == 0) {
            SET_ERR("jpeg_load_fail");
        }
        return ESP_FAIL;
    }

    char caption[256];
    build_caption(meta_json, caption, sizeof(caption));

    /* token 只打前 6 位 */
    char tok[16];
    strlcpy(tok, cfg->telegram_token, sizeof(tok));
    if (strlen(tok) > 6) {
        tok[6] = '\0';
        strlcat(tok, "***", sizeof(tok));
    }
    LOGI(TAG, "sendPhoto chat=%s len=%u caption=\"%.60s\" token=%s",
         cfg->telegram_chat_id, (unsigned)jpeg_len, caption, tok);

    esp_err_t ret = ESP_FAIL;
    static const uint32_t backoff[TG_RETRY_MAX] = {1000, 2000, 3000, 4000, 5000};
    for (int i = 0; i <= TG_RETRY_MAX; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff[i - 1]));
            /* ★ 原来这行**没打 err**，导致"照片发不出去"时完全不知道原因
             *   （真机踩到：只有 "sendPhoto retry N/3"，故障点全靠猜）。 */
            LOGW(TAG, "sendPhoto retry %d/%d after %ums err=%s", i, TG_RETRY_MAX,
                 (unsigned)backoff[i - 1], s_last_err);
        }
        ret = tg_send_photo(cfg->telegram_chat_id, caption, jpeg, jpeg_len);
        if (ret == ESP_OK) {
            s_last_send_ms = esp_timer_get_time() / 1000;
            LOGI(TAG, "sendPhoto ok retry=%d", i);
            break;
        }
    }

    free(jpeg);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* 远程命令                                                               */
/* ---------------------------------------------------------------------- */
static void tg_send_message(const char *text)
{
    config_store_t *cfg = config_store_get();
    if (cfg->telegram_chat_id[0] == '\0') {
        return;
    }
    char enc[768];
    char body[1024];
    url_encode(text, enc, sizeof(enc));
    snprintf(body, sizeof(body), "chat_id=%s&text=%s", cfg->telegram_chat_id, enc);
    char resp[256];
    tg_call_with_retry("sendMessage", body, resp, sizeof(resp), NULL);
}

/**
 * @brief 把 "/sleep@my_bot 30" 拆成 verb="/sleep" + arg="30"（arg 可为空串）
 *
 * ★ 为什么要处理 @botname 后缀：群聊里发的命令，Telegram 会自动补成
 *   `/cmd@botname`。原来全程只做 strcmp(cmd, "/xxx")，在群里**所有命令
 *   都会静默失效**（而且没有任何日志异常）。
 */
static void tg_split_cmd(const char *raw, char *verb, size_t verb_sz,
                         char *arg, size_t arg_sz)
{
    const char *p = raw ? raw : "";
    while (*p == ' ') { p++; }

    size_t o = 0;
    while (*p && *p != ' ' && *p != '@' && o + 1 < verb_sz) {
        verb[o++] = *p++;
    }
    verb[o] = '\0';

    while (*p && *p != ' ') { p++; }        /* 跳过 @botname */
    while (*p == ' ')       { p++; }        /* 跳过空格 */

    o = 0;
    while (*p && o + 1 < arg_sz) {
        arg[o++] = *p++;
    }
    arg[o] = '\0';
}

static void tg_handle_command(const char *cmd, const char *chat_id)
{
    LOGI(TAG, "command '%s' from chat=%s", cmd, chat_id);

    char verb[32];
    char arg[32];
    tg_split_cmd(cmd, verb, sizeof(verb), arg, sizeof(arg));

    if (strcmp(verb, "/snap") == 0) {
        /* ★ 必须用 TRIG_SRC_TELEGRAM：早期版本误用 TRIG_SRC_REMOTE_MQTT，
         *   导致 Telegram 触发的照片在 meta.trigger 里被写成 "remote_mqtt"，
         *   上报来源字段失真（meta 是任务书 5.4 规定的字段）。 */
        esp_err_t e = trigger_fsm_fire(TRIG_SRC_TELEGRAM, "telegram");
        tg_send_message(e == ESP_OK ? "snap queued" : "snap rejected (cooldown/armed)");
    } else if (strcmp(verb, "/sleep") == 0) {
        /* ★★ 睡眠控制（2026-09-28 用户要求，与 /nosleep 配对）★★
         *
         *   /sleep        -> 开启睡眠，间隔用**默认 60s(1min)**
         *   /sleep 30     -> 开启睡眠，间隔 30s
         *   /sleep 0      -> 开启睡眠但关掉定时唤醒（只靠 PIR/按键醒）
         *
         * 之所以直接改内存字段而不是内部去 POST /api/config：
         *   我们就在设备里，改完 config_store_save() 落盘即可，
         *   不用自己拼 JSON、也不用绕一圈 HTTP。
         * ★ 立即生效：sleep_mgr_sleep_now() 每次入睡前都重读配置。
         */
        int sec = arg[0] ? atoi(arg) : 60;      /* 不带参数 = 默认 1 分钟 */
        if (sec < 0)    { sec = 0; }
        if (sec > 3600) { sec = 3600; }         /* 与 config 的 0..3600000 边界一致 */

        config_store_t *cfg = config_store_get();
        cfg->sleep_enable  = 1;
        cfg->sleep_poll_ms = sec * 1000;
        config_store_save();

        char buf[320];
        snprintf(buf, sizeof(buf),
                 "sleep=on 间隔=%ds\n"
                 "· 入睡后按此间隔定时唤醒并轮询命令\n"
                 "· 当前若是常驻状态，会在下一次拍照/上传结束后入睡",
                 sec);
        tg_send_message(buf);
    } else if (strcmp(verb, "/nosleep") == 0) {
        /* 不睡眠（常驻）：网页/HTTP 随时可用，命令随发随到。
         * ⚠ 必须让常驻轮询任务跑起来，否则设备会变成"醒着但收不到任何命令"
         *   —— 定时唤醒那条路径本来**不创建** tg_poll 任务，main.c 检测到
         *   "没睡"时会补上（见 SLEEP_WAKE_TIMER 分支）。 */
        config_store_t *cfg = config_store_get();
        cfg->sleep_enable = 0;
        config_store_save();
        tg_send_message("sleep=off（常驻）\n"
                        "· 网页/HTTP 一直可用，命令随发随到\n"
                        "· 代价：不再省电，电池会持续消耗");
    } else if (strcmp(verb, "/status") == 0) {
        char buf[320];
        config_store_t *cfg = config_store_get();
        snprintf(buf, sizeof(buf),
                 "%s v%s\nsensor=%s sd=%s\nip=%s rssi=%d armed=%s\n"
                 "queue=%d/%d heap=%uK\nsleep=%s 间隔=%ds",
                 SNAP_FW_NAME, SNAP_FW_VERSION,
                 camera_hal_sensor_name(), sd_hal_state_str(sd_hal_state()),
                 wifi_hal_ip_str(), wifi_hal_rssi(),
                 config_store_is_armed() ? "1" : "0",
                 upload_queue_len(), cfg->max_queue_len,
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 cfg->sleep_enable ? "on" : "off",
                 cfg->sleep_poll_ms / 1000);
        tg_send_message(buf);
    } else if (strcmp(verb, "/arm") == 0) {
        config_store_set_armed(true);
        config_store_save();
        trigger_fsm_set_armed(true);
        tg_send_message("armed=1");
    } else if (strcmp(verb, "/disarm") == 0) {
        config_store_set_armed(false);
        config_store_save();
        trigger_fsm_set_armed(false);
        tg_send_message("armed=0");
    } else if (strcmp(verb, "/reboot") == 0) {
        tg_send_message("rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        tg_send_message("commands: /snap /status /arm /disarm /reboot\n"
                        "睡眠: /sleep [秒] (默认60s) · /nosleep");
    }
}

/**
 * @brief 把 offset 直接跳到"最新一条更新"—— 解析失败后的自愈
 *
 * 为什么必须有：s_update_offset 只在**解析成功**后的循环里推进（见下方）。
 *   一旦响应被截断导致 cJSON_Parse 失败，offset 就停在原处，下一次仍拿到
 *   同一批老消息、仍然超长、仍然失败 —— 死锁，而且**完全静默**。
 *
 * 实测现场（2026-09-28）：用户在 Telegram 连发 10 条 /snap，getUpdates
 *   响应 3372 字节 > 当时的 2048 缓冲；设备三个多小时里一条命令都没处理，
 *   中继日志只看到每 10s 一次成功的 TLS 握手，看着像"网络完全正常"。
 *
 * 求助方式：getUpdates 传**负 offset** 表示"返回最后 N 条更新"，
 *   配 limit=1 取回最新一条的 update_id，直接跳过去。
 *   代价是丢掉积压的命令，但比"永远收不到命令"好得多；
 *   正常路径带 limit=1，压根不会走到这里。
 */
static void telegram_poll_resync(void)
{
    char resp[1024];
    if (tg_call_with_retry("getUpdates", "offset=-1&limit=1&timeout=0",
                           resp, sizeof(resp), NULL) != ESP_OK) {
        return;              /* 自愈也失败：保持原 offset，下一轮再试 */
    }
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        return;
    }
    const cJSON *res = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsArray(res)) {
        int n = cJSON_GetArraySize(res);
        const cJSON *last = (n > 0) ? cJSON_GetArrayItem(res, n - 1) : NULL;
        const cJSON *id = last ? cJSON_GetObjectItem(last, "update_id") : NULL;
        if (cJSON_IsNumber(id)) {
            s_update_offset = id->valueint;
            LOGI(TAG, "resync ok -> offset=%d（积压命令已丢弃）",
                 (int)s_update_offset);
        }
    }
    cJSON_Delete(root);
}

static esp_err_t tg_poll_inner(void)
{
    config_store_t *cfg = config_store_get();
    if (cfg->telegram_token[0] == '\0' || cfg->telegram_chat_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char body[96];
    /* ★★ limit=1 是**防死锁**的关键，不是为了省流量 ★★
     *
     * 一次只取一条 -> 响应固定几百字节 -> 绝不可能超缓冲被截断。
     * 早期不带 limit，只要积压 10 条老命令就能凑出 3372 字节的响应
     * （实测：用户连发 10 条 /snap），超过 resp 缓冲 -> cJSON_Parse 失败
     * -> s_update_offset 不前进 -> 下次又拿到同一批 -> 永久死锁且静默。 */
    snprintf(body, sizeof(body), "offset=%lld&limit=1&timeout=0",
             (long long)(s_update_offset + 1));

    /* getUpdates 用 GET 更合适，但为复用连接配置这里用 POST */
    /* 缓冲 2048 -> 4096：单条命令约 250~350 字节，留 10 倍余量。
     * 用户若真发一条 4KB 长文本仍可能超，交给 telegram_poll_resync() 兜底。 */
    char resp[4096];
    esp_err_t e = tg_call_with_retry("getUpdates", body, resp, sizeof(resp), NULL);
    if (e != ESP_OK) {
        return e;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        /* 解析失败必须自愈，否则就是死锁（见 telegram_poll_resync 的说明）。
         * 这行日志很关键：早期版本只有 SET_ERR 不打日志，现场看起来
         * 就是"网络正常但命令没反应"，排查方向全被带偏。 */
        LOGW(TAG, "getUpdates 响应解析失败(缓冲=%uB) -> 跳到最新一条自愈",
             (unsigned)sizeof(resp));
        telegram_poll_resync();
        SET_ERR("json_parse_fail");
        return ESP_FAIL;
    }
    const cJSON *okf = cJSON_GetObjectItem(root, "ok");
    const cJSON *res = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsTrue(okf) || !cJSON_IsArray(res)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(res);
    for (int i = 0; i < n; i++) {
        const cJSON *upd = cJSON_GetArrayItem(res, i);
        const cJSON *upd_id = cJSON_GetObjectItem(upd, "update_id");
        if (cJSON_IsNumber(upd_id)) {
            s_update_offset = upd_id->valueint;
        }
        const cJSON *msg = cJSON_GetObjectItem(upd, "message");
        if (!msg) {
            continue;
        }
        const cJSON *chat = cJSON_GetObjectItem(msg, "chat");
        const cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
        const cJSON *txt = cJSON_GetObjectItem(msg, "text");

        /* 只响应来自已配置 chat_id 的命令（其他人发的直接忽略） */
        char cid_s[32] = {0};
        if (cJSON_IsNumber(cid)) {
            snprintf(cid_s, sizeof(cid_s), "%lld", (long long)cid->valuedouble);
        }
        if (strcmp(cid_s, cfg->telegram_chat_id) != 0) {
            LOGW(TAG, "drop command from unauthorized chat=%s", cid_s);
            continue;
        }
        if (cJSON_IsString(txt) && txt->valuestring) {
            tg_handle_command(txt->valuestring, cid_s);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 拉一次命令（对外唯一入口）
 *
 * ★★ 这层包装存在的唯一理由：把"本轮轮询已结束"变成一个 app_main 可观察的
 *   计数。定时唤醒路径**不能**自己调内部实现 ——
 *   app_main 的栈只有 8KB，而 getUpdates 要跑 TLS 握手 +
 *   esp_http_client + cJSON；收到 /snap 时还要再发一条 sendMessage
 *   （第二个 TLS）。实测直接爆栈：
 *       ***ERROR*** A stack overflow in task main has been detected.
 *       0x421444bd: _get_host_header (esp_http_client.c:714)
 *   紧接着 rst:0xc (RTC_SW_CPU_RST) reset=PANIC —— 表现为不停重启。
 *   所以 app_main 只负责起 tg_poll_task（栈 12288）并等这里 +1。
 *
 * 计数**无条件 +1**（成功失败都算）：否则失败那轮 app_main 会一直等到超时，
 *   白白把醒着的时间拖长。
 */
esp_err_t upload_telegram_poll_commands(void)
{
    esp_err_t e = tg_poll_inner();
    s_poll_done++;
    return e;
}

/** 轮询已完成的次数（单调递增），供 app_main 等待"这一轮收完了没有" */
int upload_telegram_poll_done_count(void)
{
    return s_poll_done;
}
