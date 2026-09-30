/**
 * @file config_store.c
 * @brief littlefs /config.json 读写
 *
 * 安全约定（任务书 0.8）：禁止把 WiFi 密码 / Telegram Token / admin_key 硬编码在源码里。
 *   - /config.json 里存的是**空字符串**，必须由用户在 AP 配网页或 POST /api/config 里填写。
 *   - 只有 device_id / 时间服务器 / 触发电平这类非密钥项有"合理默认值"。
 */

#include "config_store.h"
#include "app_conf.h"        /* ★ 部署默认值（MQTT 地址/端口/账号 + 主题前缀） */
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

static const char *TAG = LOG_T_CFG;
#define CFG_PATH        "/littlefs/config.json"
#define CFG_PATH_TMP    "/littlefs/config.json.tmp"
#define CFG_PARTITION   "littlefs"

static config_store_t s_cfg;
static bool           s_loaded;
static bool           s_safe_mode;

/* ============================== 默认值 ============================== */
static void set_defaults(config_store_t *c)
{
    memset(c, 0, sizeof(*c));

    /* device_id：用 STA MAC 后两字节生成，形如 cam-a1b2，无需用户配置 */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(c->device_id, sizeof(c->device_id), "cam-%02x%02x", mac[4], mac[5]);

    /* 密钥字段：默认值一律来自 app_conf.h。
     *   · 宏留空 -> 该字段为空 -> 运行时由用户用 /api/config 填写
     *   · 宏有值 -> 编译期生效，全批相同（见 app_conf.h 的安全权衡说明）
     * 这是工程里"硬编码密钥"的**唯一例外**，理由见 app_conf.h 文件头。 */
    snprintf(c->wifi_ssid, sizeof(c->wifi_ssid), "%s", APP_WIFI_SSID);
    snprintf(c->wifi_pass, sizeof(c->wifi_pass), "%s", APP_WIFI_PASS);
    c->admin_key[0]          = '\0';   /* 每台可不同，强制运行时设置 */
    snprintf(c->http_header_value, sizeof(c->http_header_value), "%s",
             APP_HTTP_HEADER_VALUE);
    snprintf(c->telegram_token, sizeof(c->telegram_token), "%s", APP_TELEGRAM_TOKEN);
    snprintf(c->telegram_chat_id, sizeof(c->telegram_chat_id), "%s",
             APP_TELEGRAM_CHAT_ID);

    c->armed = true;
    /* ★ 只走 Telegram：HTTP 上传目标（APP_HTTP_URL）指向的是另一张网卡，
     *   设备连不上会连刷 4 次 perform_fail_0x7004 并把 ok 计数搅乱。
     *   交付现场以 Telegram 为唯一出口，改回 both 需同时确认 http_url 可达。 */
    snprintf(c->upload_mode, sizeof(c->upload_mode), "telegram");

    /* HTTP 上传目标：默认值同样来自 app_conf.h（留空 = 不上传） */
    snprintf(c->http_url, sizeof(c->http_url), "%s", APP_HTTP_URL);
    if (APP_HTTP_HEADER_KEY[0]) {
        snprintf(c->http_header_key, sizeof(c->http_header_key), "%s",
                 APP_HTTP_HEADER_KEY);
    } else {
        /* 宏留空时保留工程内建默认名，避免出现"没有名字的头" */
        snprintf(c->http_header_key, sizeof(c->http_header_key), "X-Device-Key");
    }

    /* ---- MQTT ----
     * ★ 默认值集中在 main/app_conf.h —— 接单交付只改那一个文件。
     *   这里只是「还没有 /config.json 时」的初值；一旦用 /api/config 改过，
     *   设备 flash 里的值会覆盖它（运行时配置优先）。 */
    snprintf(c->mqtt_uri, sizeof(c->mqtt_uri), "mqtt://%s:%d",
             APP_MQTT_HOST, APP_MQTT_PORT);
    snprintf(c->mqtt_user, sizeof(c->mqtt_user), "%s", APP_MQTT_USER);
    snprintf(c->mqtt_pass, sizeof(c->mqtt_pass), "%s", APP_MQTT_PASS);

    c->pir_active_level        = 1;
    c->pir_debounce_ms         = 300;
    c->min_trigger_interval_ms = 3000;
    c->flash_before_ms         = 300;
    c->retake_on_bad           = 1;      /* 判坏补拍：默认开（正常情况零开销，只在坏帧时多拍一张） */
    c->timer_interval_ms       = 0;      /* 0 = 定时抓拍关闭（默认不改变现网行为） */
    /* 人形门控阈值：默认 30%（组件默认 0.5 对"人在镜头前就要上报"偏严） */
    c->human_score_pct         = 30;

    /* 深睡策略：默认开启（做完事就睡），每 60s 定时醒来轮询一次 Telegram，
     * 30s 硬兜底防"卡死永远不睡"。三项都可在配网页 / POST /api/config 改。 */
    c->sleep_enable            = 1;
    c->sleep_poll_ms           = 60000;
    c->sleep_max_awake_ms      = 30000;
    /* ★ 默认 0：这块板关进去就再也醒不来（只能断电），必须默认安全 */
    c->cam_powerdown_on_sleep  = 0;

    c->jpeg_quality = 12;
    c->frame_size   = CONFIG_SNAP_DEFAULT_FRAMESIZE;

    snprintf(c->queue_policy, sizeof(c->queue_policy), "drop_new");
    c->max_queue_len  = CONFIG_SNAP_QUEUE_MAX;
    c->sd_low_space_mb = 200;

    snprintf(c->timezone, sizeof(c->timezone), "CST-8");
    snprintf(c->ntp_server, sizeof(c->ntp_server), "pool.ntp.org");

    c->ota_url[0] = '\0';
    memset(c->ota_md5, '0', 32);
    c->ota_md5[32] = '\0';
}

/**
 * @brief 上电自检：把"配置上就注定失败"的组合提前喊出来
 *
 * 背景：默认值里 armed=true、upload_mode="both"。如果产线刷了统一固件，
 * 但该客户没配 Wi-Fi / 没配 MQTT，设备一上电就会**不停地**尝试上报并失败，
 * 日志被刷屏、LED 一直闪，看起来像"设备坏了"，实际只是没配网。
 *
 * 这里只告警、不改配置 —— 配置怎么改是产品决策，代码不该替用户决定。
 * 但必须让现象"一眼可读"，否则产线首检会误判成硬件故障。
 */
static void sanity_check(const config_store_t *c)
{
    const bool want_http = (strcmp(c->upload_mode, "http") == 0) ||
                           (strcmp(c->upload_mode, "both") == 0);
    const bool want_tg   = (strcmp(c->upload_mode, "telegram") == 0) ||
                           (strcmp(c->upload_mode, "both") == 0);

    if (c->wifi_ssid[0] == '\0') {
        /* 没配 Wi-Fi 时，所有"要联网"的能力都不可用 —— 这是最常见的一类误判 */
        if (c->upload_mode[0] && strcmp(c->upload_mode, "off") != 0) {
            LOGW(TAG, "wifi_ssid 为空但 upload_mode=%s -> 所有上报都会失败，"
                      "请用配网页填写 Wi-Fi", c->upload_mode);
        }
        if (c->mqtt_uri[0]) {
            LOGW(TAG, "wifi_ssid 为空但 mqtt_uri=%s -> MQTT 无法连接", c->mqtt_uri);
        }
        return;
    }

    /* Wi-Fi 有了，再逐条检查各通道是否缺参数（这些都会表现为"静默不工作"） */
    if (want_http && c->http_url[0] == '\0') {
        LOGW(TAG, "upload_mode=%s 但 http_url 为空 -> HTTP 通道跳过", c->upload_mode);
    }
    if (want_tg && (c->telegram_token[0] == '\0' || c->telegram_chat_id[0] == '\0')) {
        LOGW(TAG, "upload_mode=%s 但 telegram token/chat_id 不完整 -> Telegram 跳过",
             c->upload_mode);
    }
    if (c->armed && c->timer_interval_ms == 0 && c->min_trigger_interval_ms <= 0) {
        LOGW(TAG, "armed 但定时=0 且冷却=0 -> 只有 PIR/按键能触发");
    }
}

/* ============================== JSON 读写 ============================== */

/* 只读取"存在且类型正确"的键，其它一律保留原值 —— 这样部分 POST 也能工作 */
#define GET_STR(key, field)                                                  \
    do {                                                                     \
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);        \
        if (cJSON_IsString(it) && it->valuestring) {                          \
            strlcpy(c->field, it->valuestring, sizeof(c->field));             \
        }                                                                    \
    } while (0)

#define GET_INT(key, field, lo, hi)                                          \
    do {                                                                     \
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);        \
        if (cJSON_IsNumber(it)) {                                             \
            int v = it->valueint;                                             \
            if (v < (lo) || v > (hi)) {                                       \
                LOGW(TAG, "%s=%d 超范围[%d,%d] -> 忽略", key, v, (lo), (hi)); \
            } else {                                                          \
                c->field = v;                                                 \
            }                                                                 \
        }                                                                    \
    } while (0)

#define GET_BOOL(key, field)                                                 \
    do {                                                                     \
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);        \
        if (cJSON_IsBool(it)) {                                               \
            c->field = cJSON_IsTrue(it);                                      \
        }                                                                    \
    } while (0)

/* 受控枚举：只接受白名单值 */
static void get_enum(const cJSON *root, const char *key, char *dst, size_t dst_sz,
                     const char *const *allowed, size_t n)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it) || !it->valuestring) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(it->valuestring, allowed[i]) == 0) {
            strlcpy(dst, it->valuestring, dst_sz);
            return;
        }
    }
    LOGW(TAG, "%s=\"%s\" 不是合法取值 -> 忽略", key, it->valuestring);
}

static esp_err_t parse_into(config_store_t *c, const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return ESP_ERR_INVALID_ARG;
    }

    GET_STR("device_id", device_id);
    GET_STR("wifi_ssid", wifi_ssid);
    GET_STR("wifi_pass", wifi_pass);
    GET_STR("admin_key", admin_key);
    GET_BOOL("armed", armed);

    {
        static const char *const um[] = {"http", "telegram", "both", "off"};
        get_enum(root, "upload_mode", c->upload_mode, sizeof(c->upload_mode), um, 4);
    }

    GET_STR("http_url", http_url);
    GET_STR("http_header_key", http_header_key);
    GET_STR("http_header_value", http_header_value);
    GET_STR("telegram_token", telegram_token);
    GET_STR("telegram_chat_id", telegram_chat_id);
    GET_STR("mqtt_uri", mqtt_uri);
    GET_STR("mqtt_user", mqtt_user);
    GET_STR("mqtt_pass", mqtt_pass);

    GET_INT("pir_active_level", pir_active_level, 0, 1);
    GET_INT("pir_debounce_ms", pir_debounce_ms, 10, 10000);
    GET_INT("min_trigger_interval_ms", min_trigger_interval_ms, 100, 3600000);
    /* 定时抓拍：0 = 关闭；上界 1 小时。小于冷却值也能填，多余的触发会被
     * gate 丢弃并打 cooldown skip（属预期，不在这里硬性拒绝）。 */
    GET_INT("timer_interval_ms", timer_interval_ms, 0, 3600000);
    GET_INT("flash_before_ms", flash_before_ms, 0, 3000);
    GET_INT("retake_on_bad", retake_on_bad, 0, 1);
    /* 人形门控阈值（%）：5..95。允许运行时热调，不必重编译刷机 */
    GET_INT("human_score_pct", human_score_pct, 5, 95);
    /* 深睡策略：三项都可运行时热调（改完立即生效，下次入睡即按新值） */
    GET_INT("sleep_enable", sleep_enable, 0, 1);
    GET_INT("sleep_poll_ms", sleep_poll_ms, 0, 3600000);
    GET_INT("sleep_max_awake_ms", sleep_max_awake_ms, 0, 600000);
    GET_INT("cam_powerdown_on_sleep", cam_powerdown_on_sleep, 0, 1);

    GET_INT("jpeg_quality", jpeg_quality, 4, 63);
    GET_INT("frame_size", frame_size, 5, 21);

    {
        static const char *const qp[] = {"drop_new", "drop_oldest"};
        get_enum(root, "queue_policy", c->queue_policy, sizeof(c->queue_policy), qp, 2);
    }
    GET_INT("max_queue_len", max_queue_len, 2, 16);
    GET_INT("sd_low_space_mb", sd_low_space_mb, 0, 30000);

    GET_STR("timezone", timezone);
    GET_STR("ntp_server", ntp_server);
    GET_STR("ota_url", ota_url);
    {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "ota_md5");
        if (cJSON_IsString(it) && it->valuestring && strlen(it->valuestring) == 32) {
            strlcpy(c->ota_md5, it->valuestring, sizeof(c->ota_md5));
        }
    }
    return ESP_OK;
}

static const char *MASK = "***";

static cJSON *to_json_impl(const config_store_t *c, bool masked)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }

#define PUT_STR(key, val)  cJSON_AddStringToObject(o, key, (val))
#define PUT_INT(key, val)  cJSON_AddNumberToObject(o, key, (val))
#define PUT_BOOL(key, val) cJSON_AddBoolToObject(o, key, (val))
    /* 脱敏规则：只要字段非空就打 "***"；本来就没填的字段返回 ""，
     * 让前端能区分"没配置"和"已配置但不给看"。 */
#define PUT_SECRET(key, val) \
    cJSON_AddStringToObject(o, key, ((masked && (val)[0]) ? MASK : (val)))

    PUT_STR("device_id", c->device_id);
    PUT_STR("wifi_ssid", c->wifi_ssid);
    PUT_SECRET("wifi_pass", c->wifi_pass);
    PUT_SECRET("admin_key", c->admin_key);
    PUT_BOOL("armed", c->armed);
    PUT_STR("upload_mode", c->upload_mode);

    PUT_STR("http_url", c->http_url);
    PUT_STR("http_header_key", c->http_header_key);
    PUT_SECRET("http_header_value", c->http_header_value);
    PUT_SECRET("telegram_token", c->telegram_token);
    PUT_STR("telegram_chat_id", c->telegram_chat_id);
    PUT_STR("mqtt_uri", c->mqtt_uri);
    PUT_STR("mqtt_user", c->mqtt_user);
    PUT_SECRET("mqtt_pass", c->mqtt_pass);

    PUT_INT("pir_active_level", c->pir_active_level);
    PUT_INT("pir_debounce_ms", c->pir_debounce_ms);
    PUT_INT("min_trigger_interval_ms", c->min_trigger_interval_ms);
    PUT_INT("timer_interval_ms", c->timer_interval_ms);
    PUT_INT("flash_before_ms", c->flash_before_ms);
    PUT_INT("retake_on_bad", c->retake_on_bad);
    PUT_INT("human_score_pct", c->human_score_pct);
    PUT_INT("sleep_enable", c->sleep_enable);
    PUT_INT("sleep_poll_ms", c->sleep_poll_ms);
    PUT_INT("sleep_max_awake_ms", c->sleep_max_awake_ms);
    PUT_INT("cam_powerdown_on_sleep", c->cam_powerdown_on_sleep);

    PUT_INT("jpeg_quality", c->jpeg_quality);
    PUT_INT("frame_size", c->frame_size);
    PUT_STR("queue_policy", c->queue_policy);
    PUT_INT("max_queue_len", c->max_queue_len);
    PUT_INT("sd_low_space_mb", c->sd_low_space_mb);

    PUT_STR("timezone", c->timezone);
    PUT_STR("ntp_server", c->ntp_server);
    PUT_STR("ota_url", c->ota_url);
    PUT_STR("ota_md5", c->ota_md5);

#undef PUT_STR
#undef PUT_INT
#undef PUT_BOOL
#undef PUT_SECRET
    return o;
}

/* ============================== 文件读写 ============================== */
static esp_err_t save_locked(void)
{
    cJSON *o = to_json_impl(&s_cfg, false);
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    char *txt = cJSON_Print(o);
    cJSON_Delete(o);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }

    /* 先写 tmp 再 rename：掉电最多丢掉本次修改，不会写出半个 JSON */
    FILE *f = fopen(CFG_PATH_TMP, "wb");
    if (!f) {
        LOGE(TAG, "open tmp fail");
        free(txt);
        return ESP_FAIL;
    }
    size_t n = strlen(txt);
    size_t w = fwrite(txt, 1, n, f);
    fclose(f);
    free(txt);

    if (w != n) {
        LOGE(TAG, "write tmp short %u/%u", (unsigned)w, (unsigned)n);
        remove(CFG_PATH_TMP);
        return ESP_FAIL;
    }
    remove(CFG_PATH);                 /* rename 不会覆盖已存在文件，先删 */
    if (rename(CFG_PATH_TMP, CFG_PATH) != 0) {
        LOGE(TAG, "rename fail");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t load_or_create(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) {
        LOGI(TAG, "no config.json -> write defaults");
        return save_locked();
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) {
        fclose(f);
        LOGE(TAG, "config.json size=%ld 不合理 -> 重建默认", sz);
        return save_locked();
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOGE(TAG, "config.json 解析失败 -> 重建默认");
        return save_locked();
    }
    esp_err_t err = parse_into(&s_cfg, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }
    /* 把 clamp 后的最终值落盘，保证文件与内存一致 */
    return save_locked();
}

/* ============================== 对外 API ============================== */
esp_err_t config_store_init(void)
{
    set_defaults(&s_cfg);           /* 先建立"任何情况下都有可用配置"的基线 */

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = CFG_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        /* littlefs 挂了也不能让整机起不来：退化成"纯内存配置"，但明确告警 */
        LOGE(TAG, "littlefs mount fail err=0x%x -> 配置不持久化", err);
        s_loaded = false;
        return ESP_OK;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(CFG_PARTITION, &total, &used);

    err = load_or_create();
    s_loaded = (err == ESP_OK);
    if (!s_loaded) {
        LOGE(TAG, "load fail err=0x%x -> 用默认值继续（不持久化）", err);
    }

    LOGI(TAG, "littlefs total=%uKB used=%uKB cfg=%s",
         (unsigned)(total / 1024), (unsigned)(used / 1024),
         s_loaded ? "ok" : "volatile");

    sanity_check(&s_cfg);
    return ESP_OK;
}

config_store_t *config_store_get(void)
{
    return &s_cfg;
}

esp_err_t config_store_apply_json(const cJSON *root)
{
    esp_err_t err = parse_into(&s_cfg, root);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_loaded) {
        LOGW(TAG, "apply ok but 配置不持久化（littlefs 不可用）");
        return ESP_OK;
    }
    return save_locked();
}

esp_err_t config_store_save(void)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    return save_locked();
}

cJSON *config_store_to_json(bool masked)
{
    return to_json_impl(&s_cfg, masked);
}

esp_err_t config_store_factory_reset(void)
{
    LOGW(TAG, "factory reset: 删除 /config.json 并重建默认");
    set_defaults(&s_cfg);
    if (!s_loaded) {
        return ESP_OK;
    }
    remove(CFG_PATH);
    return save_locked();
}

void config_store_set_safe_mode(bool on)
{
    s_safe_mode = on;
}

bool config_store_is_safe_mode(void)
{
    return s_safe_mode;
}

const char *config_store_upload_mode_str(void)
{
    return s_cfg.upload_mode;
}

bool config_store_is_armed(void)
{
    return s_cfg.armed;
}

void config_store_set_armed(bool armed)
{
    s_cfg.armed = armed;
}

const char *config_store_admin_key(void)
{
    return s_cfg.admin_key;
}
