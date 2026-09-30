/**
 * @file trigger_fsm.c
 * @brief 触发状态机 + PIR 中断 + 按键接入
 *
 * 关键设计：
 *  - PIR 的 GPIO ISR **只做一件事**：把 esp_timer 时间戳投进队列。
 *    所有滤波/消抖/冷却判断都在 trigger 任务里做（ISR 里不能调用有锁的东西）。
 *  - COOLDOWN 内的 PIR 事件不是"丢弃且不可见"：计入 cooldown_skip 计数并打日志。
 *  - 触发任务串行执行整个 CAPTURE→ENQUEUE，天然保证不会并发拍照。
 */

#include "trigger_fsm.h"
#include "config_store.h"
#include "logger.h"
#include "snapshot_pipeline.h"
#include "led_hal.h"
#include "button_hal.h"
#include "pins_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"      /* esp_restart：按键长按恢复出厂后重启进 AP 配网 */

static const char *TAG = LOG_T_TRIG;

#define EVT_QUEUE_LEN   16

/**
 * 统一事件：PIR ISR / 按键 / web / telegram 全部往这一个队列投，
 * 由 trigger 任务串行处理 —— 这样"冷却判断"天然是单线程的，不会出现
 * 两个触发源同时通过 gate 然后并发拍照。
 *
 * ★ marker 字段必须与 ts_ms 分开：早期版本试图用"负数时间戳"表达外部触发，
 *   这既难读又容易和真实时间戳撞车。
 */
typedef enum {
    EVT_PIR = 0,
    EVT_REMOTE,
    EVT_BUTTON,
    EVT_TIMER,      /**< 定时抓拍（由 auto_timer_cb 周期投递） */
} evt_kind_t;

typedef struct {
    evt_kind_t kind;
    int64_t    ts_ms;       /**< EVT_PIR 用：ISR 时刻             */
    trig_src_t src;         /**< EVT_REMOTE 用：远程触发来源       */
} trig_evt_t;

static QueueHandle_t     s_evt_q;
static volatile trig_state_t s_state = TRIG_IDLE;
static volatile bool     s_armed;
static int64_t           s_last_capture_ms;
static uint32_t          s_trig_count;
static uint32_t          s_cooldown_skips;

/* 定时抓拍：节拍器 + 上次自动投递时刻（见 auto_timer_cb） */
static esp_timer_handle_t s_auto_timer;
static int64_t            s_last_auto_ms;

/* ------------------------------ 状态字符串 ------------------------------ */
const char *trig_state_str(trig_state_t st)
{
    switch (st) {
    case TRIG_IDLE:     return "IDLE";
    case TRIG_DEBOUNCE: return "DEBOUNCE";
    case TRIG_CAPTURE:  return "CAPTURE";
    case TRIG_ENQUEUE:  return "ENQUEUE";
    case TRIG_COOLDOWN: return "COOLDOWN";
    default:            return "?";
    }
}

const char *trig_result_str(trig_result_t r)
{
    switch (r) {
    case TRIG_RES_OK:         return "ok";
    case TRIG_RES_DISARMED:   return "disarmed";
    case TRIG_RES_COOLDOWN:   return "cooldown";
    case TRIG_RES_BUSY:       return "busy";
    case TRIG_RES_ERROR:      return "error";
    default:                  return "?";
    }
}

/* ------------------------------ PIR ISR ------------------------------ */
static void IRAM_ATTR pir_isr(void *arg)
{
    trig_evt_t e = {
        .kind = EVT_PIR,
        .ts_ms = esp_timer_get_time() / 1000,
        .src = TRIG_SRC_PIR,
    };
    BaseType_t hp = pdFALSE;
    /* 队列满就直接丢（说明主循环已经堵住了，ISR 里绝不能再等） */
    xQueueSendFromISR(s_evt_q, &e, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

/* ------------------------------ 定时抓拍节拍器 ------------------------------ */

/** 节拍周期。500ms 让 timer_interval_ms 的分辨率达到半秒级 */
#define AUTO_TICK_MS   500

/**
 * @brief 定时抓拍节拍器（esp_timer 周期回调）
 *
 * 实现选择：用**固定节拍轮询**，而不是"按 interval 建一个周期定时器"。
 *   原因：timer_interval_ms 可以在运行时被 POST /api/config 改掉，而本工程
 *   没有配置变更回调。节拍式实现天然自适应 —— 改完下一刻就生效，
 *   不需要重建/重启定时器，也不会残留旧周期。
 *
 * ★ 本回调跑在 esp_timer 任务上下文：**绝不能阻塞、绝不能拍照**。
 *   这里只做"到点判断 + 投队列"，真正的拍照由 trigger 任务串行执行。
 */
static void auto_timer_cb(void *arg)
{
    (void)arg;

    if (!s_evt_q) {
        return;
    }
    int interval = config_store_get()->timer_interval_ms;
    if (interval <= 0) {
        return;                       /* 0 = 关闭，什么都不做 */
    }

    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_auto_ms && (now - s_last_auto_ms) < interval) {
        return;                       /* 还没到点 */
    }
    s_last_auto_ms = now;

    trig_evt_t e = { .kind = EVT_TIMER, .ts_ms = 0, .src = TRIG_SRC_TIMER };
    /* 队列满就丢：下一拍还会再来，绝不能在这里等待 */
    if (xQueueSend(s_evt_q, &e, 0) != pdTRUE) {
        LOGW(TAG, "auto timer: event queue full -> dropped");
    }
}

static void pir_setup(void)
{
    config_store_t *cfg = config_store_get();
    int level = cfg->pir_active_level ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_PIR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (cfg->pir_active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (cfg->pir_active_level == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = level,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);      /* 已安装会返回 ESP_ERR_INVALID_STATE，忽略 */
    gpio_isr_handler_add(PIN_PIR, pir_isr, NULL);
    LOGI(TAG, "pir ready gpio=%d active_%s debounce=%dms",
         PIN_PIR, cfg->pir_active_level ? "high" : "low", cfg->pir_debounce_ms);
}

/* ------------------------------ 按键回调 ------------------------------ */
static void on_button(button_event_t ev, void *user)
{
    (void)user;
    if (ev == BTN_EV_SHORT_PRESS) {
        /* ★ 短按 = 人脸识别：投进队列后由 trigger 任务走
         *   gate_and_capture() -> snapshot_pipeline_capture() -> human_gate_take()，
         *   即"抓拍 -> 检人脸 -> 有脸才入队上传；无脸只在串口记一行"。
         *   门控本身在 snapshot_pipeline 里，且**受 CONFIG_SNAP_ENABLE_HUMAN_DETECT
         *   控制**：该开关关掉时 human_gate_take() 恒返回 true，短按会退化成
         *   "无条件拍照上传"。所以"短按人脸识别"要真正生效，必须确保
         *   sdkconfig 里 CONFIG_SNAP_ENABLE_HUMAN_DETECT=y（当前默认已开）。
         *
         * ★ 必须投队列而不是直接调 trigger_fsm_fire()：
         *   这个回调跑在 button 任务里，若它调用 fire() 的"同步兜底"分支，
         *   就会和 trigger 任务并发进 gate，冷却判断失效。 */
        trig_evt_t e = { .kind = EVT_BUTTON, .ts_ms = 0, .src = TRIG_SRC_BUTTON };
        if (xQueueSend(s_evt_q, &e, 0) != pdTRUE) {
            LOGW(TAG, "button event queue full -> dropped");
        }
    } else {
        LOGW(TAG, "long press -> factory reset");
        esp_err_t err = trigger_fsm_factory_reset();
        if (err != ESP_OK) {
            LOGW(TAG, "factory reset err=0x%x -> keeper 旧配置，不重启", err);
            return;
        }
        /* ★ 必须自己重启：config_store_factory_reset() 只清 /config.json 并
         *   重写默认值，wifi_hal 的内部状态（SSID/模式/AP 标志位）不会跟着变。
         *   不重启的话设备仍以"旧凭据 + 已启动的 STA"继续跑，
         *   既不会进入 AP 配网，用户也无法重新配置网络。
         *   HTTP 路径(h_factory_reset)是它自己 esp_restart()，所以这里只覆盖按键路径。 */
        LOGW(TAG, "factory reset ok -> reboot 进入 AP 配网");
        vTaskDelay(pdMS_TO_TICKS(300));   /* 让上面两行日志先出串口 */
        esp_restart();
    }
}

/* ------------------------------ 核心逻辑 ------------------------------ */

/**
 * @brief 冷却判断（内部统一入口）
 * @param src   触发源
 * @param note  备注
 * @return true = 允许拍照；false = 拒绝（原因已打日志）
 */
static bool gate_and_capture(trig_src_t src, const char *note)
{
    config_store_t *cfg = config_store_get();

    /* ★ safe mode（SD REQUIRED 挂载失败 / 相机初始化失败）= 不拍照。
     *   必须在这里拦，因为这是所有触发源的唯一汇聚点：
     *   原来只有 web 的 /api/snapshot 检查了 safe mode，按键和 PIR 没检查，
     *   于是出现"网页拍照返回 503、硬件按键却能拍"的自相矛盾行为。 */
    if (config_store_is_safe_mode()) {
        LOGW(TAG, "src=%s rejected: safe mode", trig_src_str(src));
        return false;
    }

    if (!s_armed) {
        LOGI(TAG, "src=%s rejected: disarmed", trig_src_str(src));
        return false;
    }

    int64_t now = esp_timer_get_time() / 1000;
    int interval = cfg->min_trigger_interval_ms;
    if (s_last_capture_ms && (now - s_last_capture_ms) < interval) {
        s_cooldown_skips++;
        LOGI(TAG, "cooldown skip count=%u src=%s (%lldms < %dms)",
             (unsigned)s_cooldown_skips, trig_src_str(src),
             (long long)(now - s_last_capture_ms), interval);
        return false;
    }

    s_state = TRIG_CAPTURE;
    s_last_capture_ms = now;
    s_trig_count++;

    LOGI(TAG, "src=%s state=CAPTURE note=%s count=%u",
         trig_src_str(src), note ? note : "-", (unsigned)s_trig_count);

    led_hal_set_state(LED_ST_CAPTURE);

    /* 走完 grab -> 落盘/入队 的完整链路 */
    bool ok = snapshot_pipeline_capture(src);
    if (!ok) {
        LOGE(TAG, "capture pipeline failed src=%s", trig_src_str(src));
        s_state = TRIG_COOLDOWN;
        return false;
    }

    s_state = TRIG_COOLDOWN;
    /* COOLDOWN 期间 LED 灭（任务书 5.10：灭=COOLDOWN），
     * 上传成功时 upload_queue 会切到 LED_ST_UPLOAD_OK（绿双闪）。 */
    led_hal_set_state(LED_ST_COOLDOWN);
    return true;
}

static void trigger_task(void *arg)
{
    (void)arg;
    trig_evt_t e;

    for (;;) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (e.kind == EVT_PIR) {
            config_store_t *cfg = config_store_get();

            /* DEBOUNCE：等到消抖窗口末尾，期间要求电平持续为有效 */
            s_state = TRIG_DEBOUNCE;
            int level = gpio_get_level(PIN_PIR);
            LOGI(TAG, "src=pir state=DEBOUNCE level=%d", level);

            bool still = true;
            int steps = cfg->pir_debounce_ms / 20;
            if (steps < 1) {
                steps = 1;
            }
            for (int i = 0; i < steps; i++) {
                vTaskDelay(pdMS_TO_TICKS(20));
                int lv = gpio_get_level(PIN_PIR);
                bool active = cfg->pir_active_level ? (lv == 1) : (lv == 0);
                if (!active) {
                    still = false;
                    break;
                }
            }
            if (!still) {
                LOGI(TAG, "src=pir debounce rejected (电平已回落)");
                s_state = TRIG_IDLE;
                continue;
            }
            gate_and_capture(TRIG_SRC_PIR, "pir");
            s_state = TRIG_IDLE;
        } else {
            /* EVT_REMOTE / EVT_BUTTON / EVT_TIMER：都在这个任务上下文里执行，
             * 因此与 PIR 完全串行，冷却判断绝对可靠 */
            s_state = TRIG_DEBOUNCE;
            /* ★ note 值进 meta.json，要能区分"谁触发的"。
             *   按键 = 板载 BOOT0，短按语义是"人脸识别"（抓拍 -> 过人脸门控 ->
             *   有脸才上传），所以这里用 face_button 而不是笼统的 button，
             *   现场看 Events/meta 时一眼知道这条是按键人脸触发的。
             *   ⚠️ 别把 note 改回 "button"：那样按键人脸触发和"其它 button 来源"
             *   在数据里就分不开了。 */
            const char *note = (e.kind == EVT_BUTTON) ? "face_button"
                             : (e.kind == EVT_TIMER)  ? "timer"
                                                      : "remote";
            LOGI(TAG, "src=%s state=DEBOUNCE note=%s", trig_src_str(e.src), note);
            gate_and_capture(e.src, note);
            s_state = TRIG_IDLE;
        }
    }
}

/* ------------------------------ 对外 API ------------------------------ */
esp_err_t trigger_fsm_init(void)
{
    s_armed = config_store_is_armed();

    s_evt_q = xQueueCreate(EVT_QUEUE_LEN, sizeof(trig_evt_t));
    if (!s_evt_q) {
        return ESP_ERR_NO_MEM;
    }
    pir_setup();

    if (button_hal_init(on_button, NULL) != ESP_OK) {
        LOGW(TAG, "button init fail -> 仅 PIR 可用");
    }

    /* 定时抓拍节拍器：常开。timer_interval_ms=0 时回调立即返回，
     * 2 次/秒的空转开销可忽略（回调只读一个 int 并提前 return）。 */
    {
        const esp_timer_create_args_t ta = {
            .callback = auto_timer_cb,
            .name     = "auto_snap",
        };
        esp_err_t te = esp_timer_create(&ta, &s_auto_timer);
        if (te == ESP_OK) {
            te = esp_timer_start_periodic(s_auto_timer, (uint64_t)AUTO_TICK_MS * 1000);
        }
        if (te != ESP_OK) {
            s_auto_timer = NULL;
            LOGW(TAG, "auto timer create/start fail err=0x%x -> 定时抓拍不可用", te);
        }
    }

    if (xTaskCreate(trigger_task, "trigger", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    LOGI(TAG, "init ok armed=%d interval=%dms timer=%dms",
         s_armed ? 1 : 0, config_store_get()->min_trigger_interval_ms,
         config_store_get()->timer_interval_ms);
    return ESP_OK;
}

esp_err_t trigger_fsm_fire(trig_src_t src, const char *note)
{
    (void)note;
    if (!s_evt_q) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ★ 只投事件，不在调用者上下文里判冷却、不拍照。
     *   原因：
     *     1) 调用者可能是 httpd 线程（POST /api/snapshot），不能占它 1~2 秒；
     *     2) 冷却判断必须单线程，否则两个触发源可能同时通过 gate 并发拍照 ——
     *        这就是"连拍 50 次"验收项最容易翻车的地方；
     *     3) web handler 的栈只有 4KB 左右，塞不下 camera grab 的调用链。
     *   代价：POST /api/snapshot 返回 202 而不是 200，语义上更准确（已受理）。 */
    trig_evt_t e = { .kind = EVT_REMOTE, .ts_ms = 0, .src = src };
    if (xQueueSend(s_evt_q, &e, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOGE(TAG, "trigger queue full -> src=%s 被丢弃", trig_src_str(src));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

trig_state_t trigger_fsm_state(void)
{
    return s_state;
}

bool trigger_fsm_in_cooldown(void)
{
    config_store_t *cfg = config_store_get();
    int64_t now = esp_timer_get_time() / 1000;
    return s_last_capture_ms && (now - s_last_capture_ms) < cfg->min_trigger_interval_ms;
}

uint32_t trigger_fsm_count(void)
{
    return s_trig_count;
}

uint32_t trigger_fsm_cooldown_skips(void)
{
    return s_cooldown_skips;
}

void trigger_fsm_set_armed(bool armed)
{
    s_armed = armed;
    LOGI(TAG, "armed=%d", armed ? 1 : 0);
}

bool trigger_fsm_is_armed(void)
{
    return s_armed;
}

esp_err_t trigger_fsm_factory_reset(void)
{
    LOGW(TAG, "factory reset requested");
    esp_err_t e = config_store_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    return e;
}
