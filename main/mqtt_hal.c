/**
 * @file mqtt_hal.c
 * @brief MQTT meta 发布（默认关闭）
 *
 * 关闭时的空实现必须能编、能链、能调用 —— 所以这里不用 #if 包住函数体，
 * 而是让每个函数直接返回 ESP_ERR_NOT_SUPPORTED / "off"。
 */

#include "mqtt_hal.h"
#include "logger.h"
#include "sdkconfig.h"

static const char *TAG = LOG_T_MQTT;

#if CONFIG_SNAP_ENABLE_MQTT
/* ---- 真正启用时才需要这些 ---- */
#include "mqtt_client.h"
#include "config_store.h"
#include "app_conf.h"        /* ★ 主题前缀 APP_MQTT_TOPIC_PREFIX */
#include "wifi_hal.h"        /* wifi_hal_sta_connected()：判断是否已拿到 IP */
#include "esp_event.h"       /* IP_EVENT_STA_GOT_IP */
#include "esp_netif.h"       /* IP_EVENT 与 ip_event_got_ip_t 的定义 */
#include "esp_mac.h"         /* esp_read_mac()：主题里要拼完整 MAC */
#include <string.h>

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[96];        /* "<前缀>/<12位MAC>/event"，见 build_topic() */

static void mqtt_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        LOGI(TAG, "connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        LOGW(TAG, "disconnected -> 内建退避重连（1/2/5/10/30s）");
        break;
    case MQTT_EVENT_ERROR:
        LOGE(TAG, "error");
        break;
    default:
        break;
    }
}

/** 客户端是否已建立（IP 事件可能多次触发，只建一次） */
static bool s_started;

/**
 * 构建上报主题：<前缀>/<12 位 MAC 大写十六进制>/event
 *   本机示例：esp32S3_CAM/288485922F44/event
 *
 * ★ 为什么用完整 MAC 而不是 device_id：
 *   device_id 只取了 MAC 的后两字节（形如 cam-2f44），**批量部署时会碰撞** ——
 *   多台设备可能发到同一个主题上。完整 MAC 全局唯一，不会撞。
 */
static void build_topic(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_topic, sizeof(s_topic), "%s/%02X%02X%02X%02X%02X%02X/event",
             APP_MQTT_TOPIC_PREFIX,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/** 真正建客户端并开始连接。只在"已经有 IP"时调用 —— 原因见 mqtt_hal_init */
static esp_err_t start_client(void)
{
    if (s_started) {
        return ESP_OK;
    }
    config_store_t *cfg = config_store_get();
    if (cfg->mqtt_uri[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_config_t c = {0};
    c.broker.address.uri = cfg->mqtt_uri;
    if (cfg->mqtt_user[0]) {
        c.credentials.username = cfg->mqtt_user;
        c.credentials.authentication.password = cfg->mqtt_pass;
    }
    c.network.reconnect_timeout_ms = 1000;      /* 之后按 2/5/10/30s 退避 */
    c.network.disable_auto_reconnect = false;

    s_client = esp_mqtt_client_init(&c);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_ev, NULL);
    s_started = true;
    build_topic();
    LOGI(TAG, "start uri=%s topic=%s", cfg->mqtt_uri, s_topic);
    return esp_mqtt_client_start(s_client);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        start_client();
    }
}

esp_err_t mqtt_hal_init(void)
{
    config_store_t *cfg = config_store_get();
    if (cfg->mqtt_uri[0] == '\0') {
        LOGW(TAG, "enabled but mqtt_uri 为空 -> skip");
        return ESP_ERR_INVALID_STATE;
    }

    /* ★ 必须等 WiFi 真的拿到 IP 再建客户端。
     *   本函数在 main.c 的启动编排里排在 wifi_hal_init() 之后，但 WiFi 是
     *   **异步**连接 —— 实测要 3~4s 才拿到 IP。若此刻直接 start()，会因为
     *   还没有路由而打出一串噪音错误，极易被误读成故障：
     *     E esp-tls: [sock=47] connect() error: Host is unreachable
     *     E transport_base: Failed to open a new connection: 32772
     *     E mqtt_client: Error transport connect
     *   ESP-MQTT 的内建重连最终能自愈，但那几行 E 会永久留在日志里。
     *   所以：已有 IP 就直接开始（热重启/重连场景），否则挂一个一次性的
     *   IP_EVENT_STA_GOT_IP 回调，等路由就绪再连。 */
    if (wifi_hal_sta_connected()) {
        return start_client();
    }

    esp_err_t e = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL);
    if (e != ESP_OK) {
        LOGW(TAG, "register IP event fail err=0x%x -> 立即尝试连接", e);
        return start_client();
    }
    LOGI(TAG, "deferred -> 等 STA 拿到 IP 后再连 %s", cfg->mqtt_uri);
    return ESP_OK;
}

bool mqtt_hal_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_hal_publish_meta(const char *meta_json)
{
    if (!s_client || !s_connected || !meta_json) {
        return ESP_ERR_INVALID_STATE;      /* 不影响本地链路，调用方只记日志 */
    }
    int id = esp_mqtt_client_publish(s_client, s_topic, meta_json, 0, 0, 0);
    if (id < 0) {
        LOGW(TAG, "publish fail topic=%s", s_topic);
        return ESP_FAIL;
    }
    LOGI(TAG, "publish ok topic=%s len=%u", s_topic, (unsigned)strlen(meta_json));
    return ESP_OK;
}

const char *mqtt_hal_state_str(void)
{
    return s_connected ? "connected" : "disconnected";
}

#else  /* ---------------- 关闭时的空实现（必须能编能链） ---------------- */

esp_err_t mqtt_hal_init(void)
{
    LOGI(TAG, "disabled (CONFIG_SNAP_ENABLE_MQTT=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

bool mqtt_hal_connected(void)
{
    return false;
}

esp_err_t mqtt_hal_publish_meta(const char *meta_json)
{
    (void)meta_json;
    return ESP_ERR_NOT_SUPPORTED;
}

const char *mqtt_hal_state_str(void)
{
    return "off";
}

#endif
