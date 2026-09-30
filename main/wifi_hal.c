/**
 * @file wifi_hal.c
 * @brief STA/AP 配网 + 断线重连
 *
 * 状态机：
 *   NONE --init--> STA_CONNECTING --(got IP)--> STA_CONNECTED
 *                        |                          |
 *                        | 60s timeout              | 断线
 *                        v                          v
 *                    AP_CONFIG  <----(30s 未连上)---- (重连中)
 *                        |
 *                        | 连上 STA
 *                        v
 *                    STA_CONNECTED（AP 保留，双栈并存：配网页面仍可访问）
 *
 * ★ 绝不在任何循环里 while(!connected) 死等 —— 全部走事件回调 + 定时器。
 */

#include "wifi_hal.h"
#include "config_store.h"
#include "logger.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_ip_addr.h"
#include "esp_mac.h"          /* MACSTR / MAC2STR 在这里（不在 esp_wifi.h） */
#include "lwip/inet.h"        /* IP4_ADDR / inet_ntoa 在这里（不在 esp_netif_ip_addr.h） */

static const char *TAG = LOG_T_WIFI;

#define STA_CONNECTED_BIT  BIT0
#define STA_FAILED_BIT     BIT1

static EventGroupHandle_t s_evt;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static TimerHandle_t      s_ap_timer;      /* STA 超时 → 开 AP */
static TimerHandle_t      s_sta_retry_timer;/* AP 期间周期性试 STA */
static volatile wifi_hal_mode_t s_mode = WIFI_MODE_NONE;
static volatile bool      s_ap_started;
static volatile bool      s_got_ip;
static TimerHandle_t      s_ap_off_timer;   /* STA 连上后延迟收掉配网热点 */
static char               s_ip[24] = "0.0.0.0";
static char               s_ssid[33];
static volatile int       s_rssi;

/* ============================== 内部工具 ============================== */
static void update_ip_str(void)
{
    esp_netif_ip_info_t ip = {0};
    if (s_got_ip && s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
    }
}

static void start_ap(void)
{
    if (s_ap_started) {
        return;
    }
    wifi_config_t apc = {0};
    strlcpy((char *)apc.ap.ssid, WIFI_AP_SSID, sizeof(apc.ap.ssid));
    strlcpy((char *)apc.ap.password, WIFI_AP_PASS, sizeof(apc.ap.password));
    apc.ap.ssid_len = strlen(WIFI_AP_SSID);
    apc.ap.channel = 1;
    apc.ap.max_connection = 4;
    apc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apc.ap.pmf_cfg.required = false;

    /* ★ 关键：AP 接口只有在"真的需要配网"时才允许存在。
     *   init 阶段默认用 WIFI_MODE_STA，AP 硬件根本没起；
     *   这里按需切成 APSTA 把 AP 拉起来。
     *   之前的写法在 init 里无条件 set_mode(WIFI_MODE_APSTA)，
     *   导致设备一上电就广播开放热点（即使凭据完全正常），
     *   既是被动攻击面，又把 2.4G 信道拖到 AP 的 channel 1。
     *
     * ★★ 顺序不能颠倒：必须先 set_mode(APSTA) 再 set_config(WIFI_IF_AP)。
     *   在 STA-only 模式下 AP 接口尚未存在，set_config(WIFI_IF_AP) 会直接
     *   返回 ESP_ERR_WIFI_MODE(0x3005) 并落到下面的 return —— 热点永远建不起来，
     *   用户既搜不到 ESP32S3-Setup，也无法完成首次配网 / 恢复出厂后的重新配网。 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        LOGE(TAG, "switch to APSTA fail err=0x%x", err);
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &apc);
    if (err != ESP_OK) {
        LOGE(TAG, "set AP config fail err=0x%x", err);
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        /* 已经是 STA 模式启动过的情况下再 start 会返回 ESP_ERR_WIFI_CONN，忽略 */
        LOGW(TAG, "wifi start for AP err=0x%x", err);
    }
    s_ap_started = true;
    s_mode = WIFI_MODE_AP_CONFIG;
    update_ip_str();

    LOGW(TAG, "STA 未连上 -> 进入 AP 配网 ssid=%s pass=%s ip=%s",
         WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_IP);
    LOGW(TAG, "请在浏览器打开 http://%s 配置 Wi-Fi", WIFI_AP_IP);
}

/** AP 配网完成后收掉热点：切回纯 STA，释放信道、消除开放热点暴露面。 */
static void stop_ap(void)
{
    if (!s_ap_started) {
        return;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        LOGW(TAG, "stop AP: set_mode(STA) fail");
        return;
    }
    s_ap_started = false;
    LOGI(TAG, "AP 配网完成 -> 关闭热点，仅保留 STA");
}

/* ============================== 事件回调 ============================== */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        LOGI(TAG, "sta connecting ssid=%s", s_ssid[0] ? s_ssid : "(none)");
        s_mode = WIFI_MODE_STA_CONNECTING;
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_got_ip = false;
        xEventGroupClearBits(s_evt, STA_CONNECTED_BIT);
        if (s_mode != WIFI_MODE_AP_CONFIG) {
            s_mode = WIFI_MODE_STA_CONNECTING;
        }
        LOGW(TAG, "disconnected reason=%d -> reconnect", d ? d->reason : -1);
        /* 内建重连：esp_wifi 会按内部退避重试，不用我们写循环 */
        esp_wifi_connect();
        update_ip_str();
        break;
    }

    case WIFI_EVENT_AP_START:
        LOGI(TAG, "ap started ssid=%s ip=%s", WIFI_AP_SSID, WIFI_AP_IP);
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *c = (wifi_event_ap_staconnected_t *)data;
        LOGI(TAG, "ap client joined " MACSTR, MAC2STR(c->mac));
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *c = (wifi_event_ap_stadisconnected_t *)data;
        LOGI(TAG, "ap client left " MACSTR, MAC2STR(c->mac));
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        s_mode = WIFI_MODE_STA_CONNECTED;
        xEventGroupSetBits(s_evt, STA_CONNECTED_BIT);
        update_ip_str();

        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
        LOGI(TAG, "connected ip=%s rssi=%d", s_ip, s_rssi);

        /* STA 通了就停掉"开 AP"的倒计时，并让"试 STA"的定时器也停下 */
        if (s_ap_timer) {
            xTimerStop(s_ap_timer, 0);
        }
        if (s_sta_retry_timer) {
            xTimerStop(s_sta_retry_timer, 0);
        }
        /* 如果之前因为没凭据 / 连不上而开了配网热点，现在连上了就收掉。
         * ★ 不能在这里 vTaskDelay：本回调跑在系统事件任务里，阻塞它会
         *   卡住所有 WiFi/IP 事件。改用一次性定时器延迟 3s 关热点，
         *   顺便给正在访问配网页面的用户留出看到"已保存"回执的时间。 */
        if (s_ap_started && s_ap_off_timer) {
            xTimerStart(s_ap_off_timer, 0);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_got_ip = false;
        xEventGroupClearBits(s_evt, STA_CONNECTED_BIT);
    }
}

/* ============================== 定时器回调 ============================== */
static void ap_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (!s_got_ip) {
        start_ap();
        /* 开了 AP 之后，每 30s 再试一次 STA（配网页面就是在这里把凭据写进去的） */
        if (s_sta_retry_timer) {
            xTimerStart(s_sta_retry_timer, 0);
        }
    }
}

/** 延迟关闭配网热点：STA 拿到 IP 且 3s 内没有掉线，就收掉 AP */
static void ap_off_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (!s_got_ip) {
        return;      /* 期间又掉线了，保留热点方便用户重新配 */
    }
    stop_ap();
}

static void sta_retry_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_got_ip) {
        return;
    }
    /* 每次重试都重新读配置：用户在 AP 页面填了新 SSID/密码后，下一轮就能生效 */
    config_store_t *cfg = config_store_get();
    if (cfg->wifi_ssid[0] == '\0') {
        return;      /* 还没填，等用户 */
    }
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK &&
        strcmp((const char *)sta.sta.ssid, cfg->wifi_ssid) == 0) {
        return;      /* 凭据没变，内建重连已经在跑了 */
    }
    LOGI(TAG, "ap 配网凭据更新 -> 重连 ssid=%s", cfg->wifi_ssid);
    esp_wifi_disconnect();
    snprintf(s_ssid, sizeof(s_ssid), "%s", cfg->wifi_ssid);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta));
    esp_wifi_connect();
}

/* ================================ init ================================ */
esp_err_t wifi_hal_init(void)
{
    s_evt = xEventGroupCreate();
    if (!s_evt) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_FAIL;
    }
    /* AP 固定网段 192.168.4.1/24（默认值就是它，这里显式写明便于文档对照） */
    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        on_ip_event, NULL, NULL));

    /* ★ 默认只开 STA。AP 是"配网专用"的应急接口，不能常开：
     *   常开等于设备持续广播一个热点（安全隐患），且 AP 会把自己的
     *   channel 强加到 STA 的射频上（就是日志里的 "ap channel adjust"）。
     *   只有两条路径会切到 APSTA：① 无凭据启动 ② STA 超时未连上。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* 凭据由我们自己的 config 管 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    config_store_t *cfg = config_store_get();
    wifi_config_t sta = {0};
    if (cfg->wifi_ssid[0]) {
        strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
        snprintf(s_ssid, sizeof(s_ssid), "%s", cfg->wifi_ssid);
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 允许开放网络 */
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    /* 重连次数放到最大，剩下的交给内置退避（不要自己写死等循环） */

    ESP_ERROR_CHECK(esp_wifi_start());

    if (cfg->wifi_ssid[0] == '\0') {
        LOGW(TAG, "未配置 wifi_ssid -> 直接进入 AP 配网");
        start_ap();
    } else {
        s_ap_timer = xTimerCreate("wifi_ap", pdMS_TO_TICKS(WIFI_STA_TIMEOUT_MS),
                                  pdFALSE, NULL, ap_timer_cb);
        if (s_ap_timer) {
            xTimerStart(s_ap_timer, 0);
        }
    }

    s_sta_retry_timer = xTimerCreate("wifi_sta_re", pdMS_TO_TICKS(30000),
                                     pdTRUE, NULL, sta_retry_timer_cb);
    s_ap_off_timer = xTimerCreate("wifi_apoff", pdMS_TO_TICKS(3000),
                                  pdFALSE, NULL, ap_off_timer_cb);
    update_ip_str();
    return ESP_OK;
}

/* ================================ 查询 ================================ */
wifi_hal_mode_t wifi_hal_mode(void)
{
    return s_mode;
}

bool wifi_hal_sta_connected(void)
{
    return s_got_ip;
}

const char *wifi_hal_ip_str(void)
{
    /* AP 模式下返回 AP 的 IP；STA 模式返回 STA 的 IP */
    if (s_ap_started && !s_got_ip) {
        return WIFI_AP_IP;
    }
    return s_ip;
}

int wifi_hal_rssi(void)
{
    if (!s_got_ip) {
        return 0;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    return s_rssi;
}

const char *wifi_hal_ssid(void)
{
    return s_ssid;
}

const char *wifi_hal_mode_str(void)
{
    switch (s_mode) {
    case WIFI_MODE_STA_CONNECTING: return "sta_connecting";
    case WIFI_MODE_STA_CONNECTED:  return "sta";
    case WIFI_MODE_AP_CONFIG:      return "ap";
    default:                       return "none";
    }
}

esp_err_t wifi_hal_set_streaming(bool streaming)
{
    /* 原因见 wifi_hal.h 的说明：省电模式会在 DTIM 间睡眠，beacon
     * interval 102.4ms 的 AP 上会造成百毫秒级停顿，足以把 MJPEG 卡到
     * httpd send 超时。这里只在推流期间切 WIFI_PS_NONE。 */
    esp_err_t err = esp_wifi_set_ps(streaming ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err == ESP_OK) {
        LOGI(TAG, "ps mode -> %s", streaming ? "none (streaming)" : "min_modem (idle)");
    } else {
        LOGW(TAG, "set ps fail err=0x%x", err);
    }
    return err;
}

esp_err_t wifi_hal_reconnect(void)
{
    config_store_t *cfg = config_store_get();
    if (cfg->wifi_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

    snprintf(s_ssid, sizeof(s_ssid), "%s", cfg->wifi_ssid);
    LOGI(TAG, "reconnect with new credentials ssid=%s", cfg->wifi_ssid);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        return err;
    }
    s_mode = WIFI_MODE_STA_CONNECTING;
    err = esp_wifi_connect();
    if (s_ap_timer) {
        xTimerStart(s_ap_timer, 0);      /* 重新计时 60s */
    }
    return err;
}
