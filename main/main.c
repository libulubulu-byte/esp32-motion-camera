/**
 * @file main.c
 * @brief app_main —— 只做启动编排，业务逻辑全在各自模块里
 *
 * 启动顺序严格按任务书 5.1：
 *  1. logger init，打印 banner
 *  2. led init：WS2812 红色快闪 = 启动中
 *  3. config_store load（无则写默认）
 *  4. sd_hal init（三态）
 *  5. camera_hal init
 *  6. wifi_hal：STA 60s 失败进 AP
 *  7. SNTP
 *  8. web_server + mDNS
 *  9. uploader task、trigger task
 * 10. [READY]，绿灯慢闪
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
/* esp_flash.h / esp_psram.h 在 IDF 5.5 里不再是顶层公共头：
 *  - flash 容量查询走 esp_flash_get_size()，头在 spi_flash/ 下，但更简单可靠的是
 *    直接用 esp_flash_default_chip（声明在 esp_flash.h 里的 spi_flash 组件）
 *  - PSRAM 容量查询走 esp_psram_get_size()，头是 esp_psram.h → 在 IDF 5.5
 *    已并入 esp_psram 组件，但必须显式 PRIV_REQUIRES esp_psram / spi_flash。
 * 这两个组件已加进 main/CMakeLists.txt 的 PRIV_REQUIRES。 */
#include "esp_flash.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"        /* esp_reset_reason() 在 IDF 5.5 由 esp_system.h 提供 */
#include "esp_private/esp_clk.h"
#include "nvs_flash.h"

#include "logger.h"
#include "version.h"
#include "pins_config.h"
#include "config_store.h"
#include "led_hal.h"
#include "buzzer_hal.h"
#include "sd_hal.h"
#include "camera_hal.h"
#include "wifi_hal.h"
#include "time_sync.h"
#include "trigger_fsm.h"
#include "snapshot_pipeline.h"
#include "upload_queue.h"
#include "web_server.h"
#include "ota_hal.h"
#include "ota_check.h"
#include "human_stub.h"
#if CONFIG_SNAP_ENABLE_HUMAN_DETECT
#include "human_detect.h"
#endif
#include "mqtt_hal.h"
#include "upload_telegram.h"   /* 只为 Telegram 命令轮询任务：/snap /status 等 */
#include "sleep_mgr.h"         /* 深睡 / 唤醒策略（PIR + BOOT0 + 定时器） */

static const char *TAG = LOG_T_BOOT;

/* ------------------------- 复位原因 → 人可读字符串 ------------------------- */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

static const char *sd_mode_str(void)
{
#if defined(CONFIG_SD_MODE_NONE)
    return "NONE";
#elif defined(CONFIG_SD_MODE_REQUIRED)
    return "REQUIRED";
#else
    return "OPTIONAL";
#endif
}

/* ------------------------------ 启动横幅 ------------------------------ */
static void print_banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    size_t psram_size = esp_psram_get_size();

    LOGI(TAG, "%s v%s sensor=%s(%s) sd_mode=%s",
         SNAP_FW_NAME, SNAP_FW_VERSION,
         camera_hal_sensor_name(), camera_hal_expected_pid_str(),
         sd_mode_str());

    LOGI(TAG, "idf=%s chip=%s rev=%d flash=%luKB psram=%luKB(%s) reset=%s",
         esp_get_idf_version(),
         (info.model == CHIP_ESP32S3) ? "ESP32-S3" : "UNKNOWN",
         info.revision,
         (unsigned long)(flash_size / 1024),
         (unsigned long)(psram_size / 1024),
         (psram_size > 0) ? "OCT" : "none",
         reset_reason_str());

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    LOGI(TAG, "mac=%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* --------------------------- NVS：Wi-Fi 驱动需要 --------------------------- */
static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* ============================ 上电自动升级 ============================
 * 为什么单独开任务，而不是直接在 app_main 里调 ota_check_on_boot()：
 *
 *  1. **不拖慢启动**：取清单是网络操作，服务器不在线时会阻塞到超时
 *     （APP_OTA_CHECK_TIMEOUT_MS）。放在 app_main 里会让 LED、/api/status
 *     这些本地能力一起等 —— 而这些恰恰是现场排障要用的。
 *
 *  2. **Wi-Fi 还没连上**：wifi_hal_init() 是**非阻塞**的（见 wifi_hal.h 注释），
 *     app_main 走到第 6 步时 STA 通常还在连。所以这里要等一次
 *     wifi_hal_sta_connected()，否��每次上电检查必然失败。
 *
 *  3. **栈要够大**：OTA 成功路径要跑 SHA-256 / mbedtls。app_main 的栈只有
 *     8KB，历史上出现过"下载到 100% 时栈溢出复位 -> 无限升级循环"。
 *     独立任务给 8KB 并在下载前就进入（ota_hal_perform 内部还会分块写），
 *     留足余量。
 */
#define OTA_CHECK_TASK_STACK   8192
#define OTA_CHECK_WIFI_WAIT_MS 60000   /* 等 STA：与 wifi_hal 的 60s 超时一致 */

static void ota_check_task(void *arg)
{
    (void)arg;

    /* 等 STA 连上。等不到不是错误：可能是 AP 配网模式（新设备还没配 Wi-Fi），
     * 这时离线检查毫无意义，直接退出，等下次上电。 */
    const TickType_t step = pdMS_TO_TICKS(500);
    int waited = 0;
    while (!wifi_hal_sta_connected() && waited < OTA_CHECK_WIFI_WAIT_MS) {
        vTaskDelay(step);
        waited += 500;
    }

    if (!wifi_hal_sta_connected()) {
        LOGI(LOG_T_OTA, "STA 未连上（AP 配网中？）-> 跳过上电版本检查");
        vTaskDelete(NULL);
        return;
    }

    LOGI(LOG_T_OTA, "STA up (ip=%s rssi=%d) -> 检查新版本",
         wifi_hal_ip_str(), wifi_hal_rssi());

    /* 失败只告警：ota_hal 保证"不写分区、不重启"，设备继续以当前版本运行 */
    ota_check_on_boot();

    vTaskDelete(NULL);
}

/** 起一个后台任务做版本检查（失败不影响主流程） */
static void ota_check_start(void)
{
#if CONFIG_SNAP_ENABLE_OTA
    BaseType_t ok = xTaskCreate(ota_check_task, "ota_check",
                                OTA_CHECK_TASK_STACK, NULL, 4, NULL);
    if (ok != pdPASS) {
        LOGW(LOG_T_OTA, "ota_check task 创建失败 -> 跳过自动升级");
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Telegram 命令轮询                                                   */
/* ------------------------------------------------------------------ */
/**
 * ★ 这个任务是为了补一处**静默的死代码**：
 *
 *   upload_telegram_poll_commands()（upload_telegram.c）早已把
 *   getUpdates + /snap /status /arm /disarm /reboot 这套命令处理完整实现了
 *   （含 offset 记忆、非授权 chat 直接丢弃），但**全工程找不到任何调用点**。
 *
 *   后果：设备能往 Telegram **发**照片，却永远**收不到**命令；
 *   而且完全静默 —— 函数从没被调用过，自然也不会留下任何日志，
 *   现场只会表现为"我给 bot 发 /snap 一点反应都没有"，
 *   极易被误判成 "token 填错了 / Telegram 坏了"。
 *
 * 这里补上唯一的调用点。未配 token/chat_id 时，该函数会立刻返回
 * ESP_ERR_INVALID_STATE（不发请求、不打日志），所以无需额外判断，也不会刷屏。
 *
 * ★ 2026-09-28：间隔从 3s 放长到 8s。
 *
 * 原设计写 3s（理由："再长会有明显的发了命令半天没反应体感"），但实测暴露出
 * 一个更要命的代价：tg_post_simple 每次调用都会**新建并销毁一整条 TLS 连接**
 * （esp_http_client_init → 完整握手 → cleanup），也就是**每 3 秒就在内部 RAM 上
 * 申请/释放约 24KB**（IN 16KB + OUT 4KB + SSL 上下文）。这种高频 churn 正是
 * 内部 RAM 碎片化的主要来源，会导致 mbedtls_ssl_setup 偶发分配失败：
 *     E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
 * 表现为照片/命令偶发失败，靠重试才发得出去。
 *
 * 8s 的取舍：本工程的主触发路径是 PIR/按键，Telegram 命令只是辅助手段，
 * 多等 5 秒可以接受；换来连接创建频率降到 1/2.7，碎片显著减少。
 */
#define TG_POLL_INTERVAL_MS   8000
#define TG_POLL_NET_WAIT_MAX  60      /* 最多等 60s 等 STA 连上 */
#define TG_POLL_TASK_STACK    12288   /* TLS 握手 + cJSON 解析 getUpdates 响应
                                       * （响应缓冲 2048->4096，栈同步加大，
                                       *   否则 8KB 栈会被吃掉一半） */

/* 定时唤醒后"等这一轮 Telegram 轮询完成"的上限（毫秒）。
 *
 * 为什么是"等轮询完成"而不是"等 STA"：
 *   轮询跑在 tg_poll_task 里（栈 12288），任务内部自己会先等 STA；
 *   app_main 只等 upload_telegram_poll_done_count() 推进。
 *
 * ✗✗ 绝对不要改回"app_main 里直接调 upload_telegram_poll_commands()" ✗✗
 *   app_main 的栈只有 8KB，getUpdates 要跑 TLS + esp_http_client + cJSON，
 *   收到 /snap 还要再发一条 sendMessage（第二个 TLS）。实测直接爆栈：
 *       ***ERROR*** A stack overflow in task main has been detected.
 *       0x421444bd: _get_host_header (esp_http_client.c:714)
 *   然后 rst:0xc (RTC_SW_CPU_RST) reset=PANIC —— 现场就是"不停重启"。
 *
 * 取 12s 的实测依据：启动到 app_main ~2s + 关联 4.4s + TLS ~2s ≈ 8~9s，
 *   留约 1.4 倍余量，且远小于 sleep_max_awake_ms(默认 30s)。 */
#define TG_WAKE_POLL_WAIT_MS  12000

static void tg_poll_task(void *arg)
{
    (void)arg;

    /* AP 配网阶段没有外网，先等 STA 连上，避免白耗 TLS 握手 */
    for (int i = 0; i < TG_POLL_NET_WAIT_MAX && !wifi_hal_sta_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        /* 掉线期间直接跳过，不做无谓的连接尝试 */
        if (wifi_hal_sta_connected()) {
            upload_telegram_poll_commands();
        }
        vTaskDelay(pdMS_TO_TICKS(TG_POLL_INTERVAL_MS));
    }
}

static void tg_poll_start(void)
{
    /* ★ 必须幂等：定时唤醒路径折回常驻时会再调一次（见 SLEEP_WAKE_TIMER
     *   分支里 /nosleep 的处理）。重复创建会得到两个轮询任务，它们共用
     *   同一个 s_update_offset，互相抢更新 => 命令会莫名其妙地丢。 */
    static bool s_started;
    if (s_started) {
        return;
    }
    BaseType_t ok = xTaskCreate(tg_poll_task, "tg_poll",
                                TG_POLL_TASK_STACK, NULL, 3, NULL);
    if (ok != pdPASS) {
        LOGW(LOG_T_TG, "tg_poll task 创建失败 -> Telegram 命令(/snap 等)不可用");
    } else {
        s_started = true;
    }
}

void app_main(void)
{
    /* 1. logger + banner ------------------------------------------------ */
    logger_init();
    print_banner();

    /* OTA 结果上报（上一轮升级成功/失败），必须在 nvs 初始化后 */
    ESP_ERROR_CHECK(init_nvs());

    /* 2. LED：WS2812 红色快闪 = 启动中 ---------------------------------- */
    ESP_ERROR_CHECK(led_hal_init());
    led_hal_set_state(LED_ST_BOOT);

    /* 2b. 蜂鸣器：必须早于 sd_hal_init —— REQUIRED 挂载失败会立刻进 FATAL，
     *     而 led_hal 在 FATAL 时会联动蜂鸣器报警，此时它必须已初始化完毕。
     *     失败不致命：蜂鸣器是可选外设（有些板子没焊）。 */
    if (buzzer_hal_init() != ESP_OK) {
        LOGW(LOG_T_SYS, "buzzer unavailable -> 仅用 LED 指示");
    }

#if CONFIG_SNAP_ENABLE_OTA
    ota_hal_report_last_result();   /* 打 [OTA] last_result=ok/fail */
#endif

    /* 3. 配置：littlefs /config.json ------------------------------------- */
    ESP_ERROR_CHECK(config_store_init());
    config_store_t *cfg = config_store_get();
    LOGI(LOG_T_CFG, "load ok device_id=%s", cfg->device_id);

    /* 3b. 深睡策略：认出本次唤醒来源 + 挂硬兜底看门狗。
     *
     * ★ 必须**早于各驱动 init**：深睡唤醒后 GPIO0/GPIO1 仍挂在 RTC IO 上，
     *   本函数会把它们交还给普通 GPIO 驱动。不先做这一步，
     *   button_hal / trigger_fsm 的 gpio_config 可能配不上，
     *   现场表现为"PIR 和按键都没反应"。 */
    ESP_ERROR_CHECK(sleep_mgr_init());

    /* 4. SD：三态 + 状态机 ---------------------------------------------- */
    esp_err_t sd_err = sd_hal_init();
    if (sd_err != ESP_OK && sd_hal_is_required()) {
        /* REQUIRED 模式挂载失败：红灯常亮 + 蜂鸣器，只保留 safe web 配网，不拍照 */
        LOGE(LOG_T_SD, "REQUIRED mount fail err=0x%x -> safe mode (web only)", sd_err);
        led_hal_set_state(LED_ST_FATAL);
        config_store_set_safe_mode(true);
    }

    /* 5. 摄像头 --------------------------------------------------------- */
    esp_err_t cam_err = camera_hal_init();
    if (cam_err != ESP_OK) {
        LOGE(LOG_T_CAM, "init failed err=0x%x -> safe mode (web only)", cam_err);
        /* SD_REQUIRED 失败 / camera 失败 都属于致命错误：红灯常亮 */
        led_hal_set_fatal_or(LED_FATAL_CAMERA);
        config_store_set_safe_mode(true);
    }

    /* 6. Wi-Fi：STA 60s 失败进 AP 配网 ----------------------------------- */
    ESP_ERROR_CHECK(wifi_hal_init());

    /* 7. SNTP：失败 time_valid=false 继续运行 ----------------------------- */
    time_sync_init();

    /* 8. web server + mDNS ---------------------------------------------- */
    ESP_ERROR_CHECK(web_server_init());

#if CONFIG_SNAP_ENABLE_MQTT
    mqtt_hal_init();
#endif

#if CONFIG_SNAP_ENABLE_HUMAN_STUB
    human_stub_init();
#endif

#if CONFIG_SNAP_ENABLE_HUMAN_DETECT
    /* 8b. 人形检测模型：**在起 trigger 任务之前**加载完。
     *
     * 为什么不等第一张照片到来时懒加载：
     *   模型加载要几百毫秒，且会从 PSRAM 里要 ~1MB。如果放在第一次抓拍里做，
     *   那一次抓拍的耗时会突然变成 1 秒以上，看起来像"相机卡住了"，
     *   而且第一次门控的结果无法与后续对比。
     *
     * 失败不算致命：human_detect_run() 内部是 fail-open，
     * 加载失败只会退化成"不做门控、全部上传"，不会让设备变砖。
     * 但**必须显式打出来**，否则现场会以为"检测开着的，怎么一张都没丢"。 */
    {
        esp_err_t hd_err = human_detect_init(0);
        if (hd_err != ESP_OK) {
            LOGW(LOG_T_SYS, "human detect init fail err=0x%x -> 门控失效，所有照片都会上传",
                 hd_err);
        } else {
            LOGI(LOG_T_SYS, "human detect ready: 只有检出人脸的帧才会入队上传");
        }
    }
#endif

    /* 9. 上传队列 + 拍照管道 + 触发状态机 -------------------------------- */
    ESP_ERROR_CHECK(upload_queue_init());
    ESP_ERROR_CHECK(snapshot_pipeline_init());
    ESP_ERROR_CHECK(trigger_fsm_init());       /* 内部会订阅 PIR / 按键 */

    /* 9b. 上电自动升级检查：起后台任务，不阻塞启动。
     *
     * ★ 必须放在下面 safe mode 早退**之前**。
     *   直觉上"设备坏了（SD/摄像头故障）就不该联网升级"，但恰好相反：
     *   safe mode 意味着设备功能不完整，**正是最需要靠 OTA 修复的场景**。
     *   若放在早退之后，一台 SD 卡损坏的设备将永远停在旧固件上 ——
     *   而它连"新固件已修复该问题"这件事都无从得知。
     *
     * 放在第 9 步之后而不是更早，是让 MQTT/上报通道先跑起来：
     * 检查任务只在自己的栈上等网络，不与业务任务抢启动时机。 */
    /* ★ 2026-09-28：**只冷启动做版本检查**。
     *   深睡形态下每次唤醒（PIR/按键/定时）都查一次版本是纯浪费：
     *   多等几秒网络，而这些唤醒根本不需要它。 */
    if (sleep_mgr_is_cold_boot()) {
        ota_check_start();
    } else {
        LOGI(LOG_T_OTA, "非冷启动(wake=%s) -> 跳过上电版本检查",
             sleep_mgr_wake_str());
    }

    /* 9c. Telegram 命令轮询：让 /snap /status /arm /disarm /reboot 真正可用。
     *
     * ★ 与 OTA 同理放在 safe mode 早退**之前**：设备出故障时，正是最需要
     *   远程 /status 看一眼、/reboot 重来一次的时候。
     *   （/snap 在 safe mode 下没有拍照链路，会被 FSM 正常拒绝并回一句提示。） */
    /* ★ 定时唤醒本来就是"为了轮询一次命令"而醒的，不要再起常驻轮询任务
     *   （下面 9d 里跑一次就回睡，避免多开一条任务 + 多余 TLS 连接）。 */
    if (sleep_mgr_wake_cause() != SLEEP_WAKE_TIMER) {
        tg_poll_start();
    }

    /* 9d. 按唤醒源分派本次任务 -------------------------------------------
     *
     * 深睡形态下"醒来"是有具体目的，这里显式分派。
     * ★ 放在 safe mode 早退**之前**（与 OTA/命令轮询同理）：坏设备更需要
     *   能远程响应一次，而不是无声无息地睡回去。
     *
     *   · PIR    -> 立刻触发一次抓拍（门控通过才上传，见 snapshot_pipeline）
     *   · 定时   -> 只轮询一次命令行，随后回睡
     *   · BOOT0  -> **不在这里派发**：交给 button_hal 的按键语义（它在松开
     *               时判短按 = 人脸识别；长按 5s = 恢复出厂）
     *   · 冷启动 -> 什么都不做，走常驻形态（何时入睡由兜底看门狗决定）
     */
    switch (sleep_mgr_wake_cause()) {
    case SLEEP_WAKE_PIR:
        if (config_store_is_safe_mode()) {
            LOGW(LOG_T_SYS, "PIR 唤醒但处于 safe mode（无拍照链路）-> 直接回睡");
            sleep_mgr_notify_idle();
        } else {
            LOGI(LOG_T_SYS, "PIR 唤醒 -> 立即触发一次抓拍");
            trigger_fsm_fire(TRIG_SRC_PIR, "wake_pir");
        }
        break;

    case SLEEP_WAKE_TIMER: {
        /* ★★ 2026-09-28 修掉"定时唤醒轮询"的两个真 bug ★★
         *
         * bug 1 —— 轮询从来没执行过：
         *   wifi_hal_init() 是**非阻塞**的（wifi_hal.h:34 "立即返回"），
         *   醒来后立刻判 wifi_hal_sta_connected() 恒为 false，于是
         *   upload_telegram_poll_commands() 被永远跳过。现场表现：
         *   设备睡着时发的 /snap /status 永远收不到，中继日志里
         *   定时唤醒期间零活动（实测 3.5 小时一次连接都没有）。
         *   修法：交给 tg_poll_task 去轮询，app_main 等它的完成计数
         *        （见 TG_WAKE_POLL_WAIT_MS）。
         *   ★ 注意：**不能**在 app_main 里直接调轮询 —— 8KB 栈会爆栈
         *     panic 复位，生成"刚睡就醒、不停重启"的假象（已踩过）。
         *
         * bug 2 —— 轮询到了也传不出去：
         *   原代码无条件调 sleep_mgr_notify_idle()。而 /snap 走的是
         *   trigger_fsm_fire() 投队列（trigger_fsm.c:379 异步），
         *   抓拍 ~600ms + 上传数秒，notify_idle 300ms 后就把设备睡死。
         *   修法：只有"本轮确实没触发抓拍"才回睡；触发了就交给
         *   抓拍/上传链路 —— 它跑完会自己调 notify_idle。
         */
        LOGI(LOG_T_SYS, "定时唤醒 -> 起轮询任务收一次 Telegram 命令");

        /* fire() 只投队列、不同步执行，所以先记基线 */
        const uint32_t n_before = trigger_fsm_count();
        /* ★★ 轮询**必须**由 tg_poll_task 跑，不能在 app_main 里直接调 ★★
         *   app_main 的栈只有 8KB，而 getUpdates 要跑 TLS + esp_http_client
         *   + cJSON，收到 /snap 还要再发一条 sendMessage —— 实测爆栈：
         *       ***ERROR*** A stack overflow in task main
         *         0x421444bd: _get_host_header (esp_http_client.c:714)
         *       -> rst:0xc (RTC_SW_CPU_RST) reset=PANIC
         *   现场表现就是"刚睡就醒、不停重启"。tg_poll_task 的栈是 12288，
         *   这件事本来就该它干。顺便：/nosleep 让设备常驻时也靠它继续轮询，
         *   所以这里无条件起（tg_poll_start 幂等）。 */
        const int p_before = upload_telegram_poll_done_count();
        tg_poll_start();

        /* 等这一轮轮询真正跑完（任务内部自己等 STA，这里只等结果）。
         * 上限 12s：实测 启动~2s + 关联 4.4s + TLS ~2s ≈ 8~9s。 */
        int waited = 0;
        while (upload_telegram_poll_done_count() == p_before &&
               waited < TG_WAKE_POLL_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        if (upload_telegram_poll_done_count() == p_before) {
            LOGW(LOG_T_SYS, "等轮询完成超时(%dms) -> 本轮不轮询，直接回睡",
                 TG_WAKE_POLL_WAIT_MS);
        }

        /* 给 trigger 任务最多 150ms 把事件取走并进入 CAPTURE
         * （trigger 任务优先级 6，实测几毫秒内就取走；150ms 是余量）。
         * 改前是 500ms —— 那是每次"没触发任何命令"的唤醒都要白付的时间。 */
        for (int i = 0; i < 3 && trigger_fsm_count() == n_before; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (trigger_fsm_count() == n_before) {
            /* 本轮无事可做。sleep_enable=0 时 notify_idle 直接返回，
             * 继续走下面的常驻流程（tg_poll 任务上面已经起好，不用再补）。 */
            sleep_mgr_notify_idle();
        } else {
            LOGI(LOG_T_SYS, "本轮触发了抓拍 -> 交给拍照/上报链路，跑完自动回睡");
        }
        break;
    }

    default:
        break;
    }

    if (config_store_is_safe_mode()) {
        /* safe mode 下不进入正常拍照链路，LED 由 LED_ST_FATAL 持续表达 */
        LOGW(LOG_T_SYS, "safe mode: camera pipeline disabled, web config only");
        return;
    }

    /* 10. [READY]，绿灯慢闪 --------------------------------------------- */
    led_hal_set_state(LED_ST_READY);
    LOGI(LOG_T_SYS, "heap=%u psram=%u armed=%d upload=%s",
         (unsigned)esp_get_free_heap_size(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         config_store_get()->armed,
         config_store_upload_mode_str());
}
