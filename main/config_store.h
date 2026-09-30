/**
 * @file config_store.h
 * @brief littlefs /config.json 配置持久化（任务书 5.8）
 *
 * 读写都经 cJSON，写入用"先写 .tmp 再 rename"避免掉电损坏。
 * 脱敏只发生在序列化给 web 的那一份（config_store_to_json_masked），
 * 内存里的真实值不动。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_STR      128
#define CFG_MAX_URL      256
#define CFG_MAX_DEV_ID   32

typedef struct {
    /* --- 身份 --- */
    char     device_id[CFG_MAX_DEV_ID];

    /* --- Wi-Fi --- */
    char     wifi_ssid[CFG_MAX_STR];
    char     wifi_pass[CFG_MAX_STR];

    /* --- 管理 --- */
    char     admin_key[CFG_MAX_STR];
    bool     armed;
    char     upload_mode[16];         /**< "http" / "telegram" / "both" / "off" */

    /* --- HTTP 上传 --- */
    char     http_url[CFG_MAX_URL];
    char     http_header_key[CFG_MAX_STR];
    char     http_header_value[CFG_MAX_STR];

    /* --- Telegram --- */
    char     telegram_token[CFG_MAX_URL];
    char     telegram_chat_id[CFG_MAX_STR];

    /* --- MQTT --- */
    char     mqtt_uri[CFG_MAX_URL];
    char     mqtt_user[CFG_MAX_STR];
    char     mqtt_pass[CFG_MAX_STR];

    /* --- 触发 --- */
    int      pir_active_level;        /**< 1 = 高有效, 0 = 低有效 */
    int      pir_debounce_ms;         /**< 默认 300 */
    int      min_trigger_interval_ms; /**< 默认 3000 */
    int      flash_before_ms;         /**< 默认 300 */
    /**
     * "判为坏帧就补拍一张"（0/1，默认 1）。
     *
     * 判据是 **JPEG 体积**：与"最近一张合格帧"比较，低于它的 60% 即视为坏帧，
     * 立刻再拍一张，最后取两帧里更大的那张。实现见 snapshot_pipeline.c 的 grab 段
     * （0.1.8 起用来替代 0.1.5 的"连拍 3 帧取最大"）。
     *
     * 代价：正常情况**零额外开销**（只拍一张），坏帧时才多花 ~20ms。
     *
     * ⚠️ 它能救的是**瞬态坏帧**（AE 未收敛 / 驱动首帧 / 整片纯色），
     *    **救不了运动模糊** —— 第二张的曝光时间与运动速度都没变，一样糊。
     *    要改善夜里移动目标的糊，调 flash_before_ms 与曝光，别指望补拍。
     * 0 = 关闭（每次触发只拍一张，最快）。
     */
    int      retake_on_bad;
    /**
     * 定时抓拍间隔（毫秒）。0 = 关闭（默认）。
     *
     * ★ 走的是和 PIR/按键**同一条串行 gate**，因此同样受 armed 与
     *   min_trigger_interval_ms 约束：若本值小于冷却间隔，多出的触发会被
     *   丢弃并打 "cooldown skip"，属预期行为（不是故障）。
     *   触发源写进 meta.trigger = "timer"。
     */
    int      timer_interval_ms;

    /* --- 人形检测（门控） --- */
    /**
     * 人脸/人形检测的置信度阈值，单位 **百分比**（5..95，默认 30）。
     *
     * 只有 score ≥ 本值才认为"有人形"，照片才会上报（见 human_detect.cpp）。
     * · 调小 => 更灵敏，人站得远 / 侧脸 / 暗光也能过（但花纹更易误报）
     * · 调大 => 更严格，只认清晰正脸
     *
     * ⚠️ 底层模型是 ESP-WHO 的**人脸**检测（MSRMNP_S8_V1，内部输入 160x120），
     *    不是全身人形检测：人脸在画面里得足够大（约 ≥1/8 画幅宽）才检得到。
     *    想检更远的人，先把 frame_size/分辨率与补光调好，再降本阈值。
     */
    int      human_score_pct;

    /* --- 深睡 / 唤醒策略（2026-09-28 新增） --- */
    /**
     * 深睡总开关（0/1，**默认 1 = 开**）。
     *
     * 开着时设备"做完事就进 deep sleep"，靠 **PIR / BOOT0 / 定时器** 三个源唤醒。
     *
     * ⚠️⚠️ 调试或要在配网页上连续操作前**务必先置 0**，否则设备会在
     *      `sleep_max_awake_ms` 到点后自动入睡 —— 网页立刻失联、看不出原因。
     *      叫醒它：动一下 PIR，或按一下 BOOT0（板载 BOOT0 就是唤醒键）。
     */
    int      sleep_enable;

    /**
     * 定时唤醒间隔（毫秒，**默认 60000**；**0 = 关闭定时唤醒**）。
     *
     * 醒来后只做一件事：**轮询一次 Telegram 命令**（让 /snap /status 等
     * 远程命令在"常睡"形态下依然可用），做完立刻回睡。
     *
     * ⚠️ 每次唤醒都是**完整重启**（重跑 bootloader + 重连 WiFi + 重初始化
     *    摄像头 ≈ 2~4s），所以别设太小：< 5s 时醒着的时间比睡着还长，
     *    整体功耗反而**高于常驻运行**。真想省电就靠 PIR/BOOT0 事件唤醒。
     */
    int      sleep_poll_ms;

    /**
     * 唤醒后最长清醒时间（毫秒，**默认 30000**；**0 = 不设兜底**）。
     *
     * **硬兜底**：从醒来的那一刻计时，到点无条件回睡（不等待业务做完）。
     * 这条必须有 —— 否则一旦 WiFi 关联或上传卡死，设备会永远醒着把电池耗光。
     */
    int      sleep_max_awake_ms;

    /**
     * 睡前是否把摄像头**软件关进低功耗**（0/1，**默认 0 = 关**）。
     *
     * ⚠️⚠️ 这块板实测有风险，默认必须是 0 ⚠️⚠️
     *   摄像头模组 3.3V 是常电、CAM_PIN_RESET/PWDN 又都没引到 GPIO，
     *   一旦关进去，**只有"彻底断电"才能救回来**（软件清 bit6、
     *   deinit+重新 init 都试过，无效）。所以默认关着，避免"一睡就再也拍不了照"。
     *   打开前请确认 camera_hal_init() 里的唤醒逻辑真的能让它恢复出帧。
     *
     * 打开方式（无需重烧）：配网页或 python _cfgset.py cam_powerdown_on_sleep=1
     */
    int      cam_powerdown_on_sleep;

    /* --- 图像 --- */
    int      jpeg_quality;            /**< 4..63，默认 12 */
    int      frame_size;              /**< framesize_t 枚举，默认 10(SVGA) */

    /* --- 队列 / SD --- */
    char     queue_policy[16];        /**< "drop_new" / "drop_oldest" */
    int      max_queue_len;           /**< 2..16，默认 8 */
    int      sd_low_space_mb;         /**< 默认 200 */

    /* --- 时间 --- */
    char     timezone[16];            /**< 默认 "CST-8" */
    char     ntp_server[CFG_MAX_STR]; /**< 默认 "pool.ntp.org" */

    /* --- OTA --- */
    char     ota_url[CFG_MAX_URL];
    char     ota_md5[40];             /**< 32 hex + '\0' */
} config_store_t;

/** 挂载 littlefs 并加载 /config.json（不存在则写默认） */
esp_err_t config_store_init(void);

/** 取只读配置指针（永不为 NULL，init 之前返回默认值的静态副本） */
config_store_t *config_store_get(void);

/**
 * @brief 用 JSON 覆盖配置（只覆盖出现过的键）
 * @note  会做范围 clamp 与类型校验；非法值打警告并忽略该键。
 */
esp_err_t config_store_apply_json(const cJSON *root);

/** 把当前配置写回 /config.json */
esp_err_t config_store_save(void);

/** 序列化内存配置；masked=true 时把密钥字段替换为 "***" */
cJSON *config_store_to_json(bool masked);

/** 恢复出厂设置（删除 /config.json 后重建默认并保存） */
esp_err_t config_store_factory_reset(void);

/** safe mode 标志（不持久化：每次上电按实际探测结果重新判定） */
void    config_store_set_safe_mode(bool on);
bool    config_store_is_safe_mode(void);

/** upload_mode 的展示字符串（供 banner / /api/status） */
const char *config_store_upload_mode_str(void);

/* ------------------------------ 便捷取值 ------------------------------ */
bool config_store_is_armed(void);
void config_store_set_armed(bool armed);
const char *config_store_admin_key(void);

#ifdef __cplusplus
}
#endif
