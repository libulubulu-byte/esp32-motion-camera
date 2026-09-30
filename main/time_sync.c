/**
 * @file time_sync.c
 * @brief SNTP + TZ 设置
 *
 * 注意：SNTP 必须在 Wi-Fi 之后、且不能阻塞启动。这里用 esp_sntp 的
 * "非阻塞"APIs（esp_sntp_init/set_operating_mode），同步结果由回调通知。
 */

#include "time_sync.h"
#include "config_store.h"
#include "logger.h"
#include "wifi_hal.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_SYS;

static volatile bool s_valid;
static char          s_tz[16];
static bool          s_started;
static esp_timer_handle_t s_poll_timer;

static void on_sync(struct timeval *tv)
{
    s_valid = true;
    time_t now = tv->tv_sec;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    LOGI(TAG, "sntp ok tz=%s time=%s", s_tz, buf);
}

/* 非阻塞轮询回调：SNTP 回调本身在 lwIP 线程，这里是低优先级 task 里检查 */
static void poll_cb(void *arg)
{
    (void)arg;
    if (s_valid) {
        if (s_poll_timer) {
            esp_timer_stop(s_poll_timer);
        }
        return;
    }
    time_t now = 0;
    time(&now);
    /* epoch > 2020-01-01 认为同步成功（SNTP 库在没同步时返回 1970） */
    if (now > 1577836800) {
        s_valid = true;
        LOGI(TAG, "sntp ok tz=%s (by poll)", s_tz);
        if (s_poll_timer) {
            esp_timer_stop(s_poll_timer);
        }
    }
}

esp_err_t time_sync_init(void)
{
    config_store_t *cfg = config_store_get();
    strlcpy(s_tz, cfg->timezone[0] ? cfg->timezone : "CST-8", sizeof(s_tz));

    /* 1) TZ */
    setenv("TZ", s_tz, 1);
    tzset();

    /* ★★ 2026-09-28：时钟跨深睡是**保留**的（RTC 域不掉电；实测唤醒后系统
     *   时间只落后 PC 约 6 秒），所以醒来那一刻时间就已经可用。
     *
     *   time_valid 的语义应当是"当前有没有可用的时间"，而不是"本次上电 SNTP
     *   回过包没有"。原先要等 SNTP 回包才置 true，于是每次唤醒的前几秒都报
     *   time_valid=false —— 看着像"时间没同步"，实际时间是对的；而 TLS 证书
     *   校验用的是系统时间，压根不受这个标志影响（这点我一开始也判断错了）。
     *
     *   ★ 为什么这里只是"提前置标志"而**不跳过 SNTP**：
     *     唤醒次数多时若不校正，RTC 漂移会累积（实测 ~0.4%/小时量级）。
     *     SNTP 本身非阻塞、不在关键路径上，留着它是纯收益。
     */
    time_t now0 = 0;
    time(&now0);
    if (now0 > 1577836800) {              /* > 2020-01-01，与 poll_cb 判据一致 */
        s_valid = true;
        LOGI(TAG, "time carried over tz=%s epoch=%lld -> 已有可用时间"
                  "（SNTP 仍启动以校正漂移）", s_tz, (long long)now0);
    }

    /* 2) SNTP */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    if (strlen(cfg->ntp_server) > 0) {
        esp_sntp_setservername(0, cfg->ntp_server);
    } else {
        esp_sntp_setservername(0, "pool.ntp.org");
    }
    /* 备用服务器：主服务器不可达时自动切换 */
    esp_sntp_setservername(1, "time.windows.com");
    esp_sntp_setservername(2, "ntp.aliyun.com");

    esp_sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    s_started = true;

    /* 3) 非阻塞兜底轮询：60s 内看一眼，没同步就报 time_valid=false 继续跑 */
    esp_timer_create_args_t t = {
        .callback = poll_cb,
        .name = "sntp_poll",
    };
    if (esp_timer_create(&t, &s_poll_timer) == ESP_OK) {
        esp_timer_start_periodic(s_poll_timer, 5 * 1000 * 1000ULL);
    }

    LOGI(TAG, "sntp start server=%s tz=%s", cfg->ntp_server, s_tz);
    return ESP_OK;
}

void time_sync_request(void)
{
    if (s_started) {
        esp_sntp_restart();
    }
}

bool time_sync_valid(void)
{
    return s_valid;
}

const char *time_sync_tz(void)
{
    return s_tz;
}
