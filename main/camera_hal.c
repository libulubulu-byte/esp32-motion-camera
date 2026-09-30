/**
 * @file camera_hal.c
 * @brief 摄像头三传感器编译期切换 + 统一的 grab/return 路径
 *
 * 设计要点：
 *  1) **只填当前选中的那一款 sensor 配置表**，其余全部 #if 掉 —— 避免
 *     esp32-camera 的弱符号探测表把三款驱动全链进来。
 *  2) frame_size 超上限 → clamp + 警告（不静默）。
 *  3) OV5640 默认 clamp 到 XGA：5MP 单帧 JPEG 最大 ~1.5MB，
 *     我们的 PSRAM 队列最多 8 项，全 5MP 会直接 OOM。
 *  4) fb_count=2 + fb_location=PSRAM ：双缓冲允许 DMA 采下一帧时 CPU 处理上一帧，
 *     这是"连拍不丢帧"的前提。
 *  5) 每个 fb 只有一个归还路径（camera_hal_return），main 之外的模块禁止直接
 *     esp_camera_fb_return()。
 */

#include "camera_hal.h"
#include "pins_config.h"
#include "logger.h"
/* camera_hal_init() 要读运行期的 frame_size / jpeg_quality（0.1.8 起，见 init 里的注释）。
 * config_store 不依赖本模块，所以这里不会形成环。 */
#include "config_store.h"

#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"     /* esp_psram_get_size()：需 PRIV_REQUIRES esp_psram */
#include "esp_timer.h"     /* esp_timer_get_time()：判 fb 是否"本次请求之后才曝光" */

static const char *TAG = LOG_T_CAM;

/* ============================ 编译期传感器选择 ============================ */

#if defined(CONFIG_CAM_OV2640)
#define SENSOR_NAME        "OV2640"
#define SENSOR_PID         0x2642
#define SENSOR_MAX_FS      FRAMESIZE_UXGA   /* 枚举值 15（旧注释误写 13：
                                             * 2.1.7 的枚举比老教程多了
                                             * 128X128 / 320X320 / 240X240） */
#define SENSOR_HAS_AF      (0)
#elif defined(CONFIG_CAM_OV3660)
#define SENSOR_NAME        "OV3660"
#define SENSOR_PID         0x3660
/* ★ OV3660 是 3MP(2048x1536)，对应 esp32-camera 2.1.7 的 FRAMESIZE_QXGA。
 *   **没有 FRAMESIZE_3MP 这个枚举**（那是别的分支/别的库的叫法）；
 *   2.1.7 里有的是 FRAMESIZE_P_3MP（864x1536，竖屏）与 FRAMESIZE_QXGA（2048x1536）。 */
#define SENSOR_MAX_FS      FRAMESIZE_QXGA
#define SENSOR_HAS_AF      (0)
#elif defined(CONFIG_CAM_OV5640)
#define SENSOR_NAME        "OV5640"
#define SENSOR_PID         0x5640
#define SENSOR_MAX_FS      FRAMESIZE_QSXGA  /* 枚举值 23（旧注释误写 17） */
/* OV5640 AF 版本才有自动对焦；驱动会在 probe 时读寄存器判断。
 * 这里 CONSERVATIVE 地按 1 处理：允许上层询问，实际支持与否由驱动打印。 */
#define SENSOR_HAS_AF      (1)
#else
#error "Kconfig 没有选中任何 CAMERA_SENSOR，请检查 main/Kconfig.projbuild"
#endif

/* 任务书 5.2 提供的"PID 字符串"。esp32-camera 的 PID 一律以 0x 前缀、4 位十六进制给出 */
static char s_pid_str[8] = "0x0000";

/* ================================ 状态 ================================ */
static bool     s_inited;
static uint16_t s_pid;
static int      s_framesize;        /* 当前生效的 framesize */
static int      s_framesize_init;   /* init 时的 framesize —— framebuffer 是按它分配的，
                                     * 所以"改大"要先重启（见 camera_hal_apply_runtime） */
static int      s_quality;          /* 当前生效的 JPEG 质量（实际值，供 /api/status） */

/* ============================== 内部工具 ============================== */

/** framesize_t 枚举 → 人可读名（用于日志与 /api/status） */
const char *camera_hal_framesize_name(int fs)
{
    switch (fs) {
    /* ★ 顺序必须与 esp32-camera 2.1.7 driver/include/sensor.h 的 framesize_t
     *   枚举**逐一对应**。2.1.7 在 QQVGA 之后多了 FRAMESIZE_128X128，
     *   在 QVGA 之后多了 FRAMESIZE_320X320 —— 漏掉这两个（旧版教程就是这么写的）
     *   会让 VGA 之后的每一项编号整体错位一位，且这两档显示成 "?"。 */
    case FRAMESIZE_96X96:    return "96x96";
    case FRAMESIZE_QQVGA:    return "QQVGA(160x120)";
    case FRAMESIZE_128X128:  return "128x128";
    case FRAMESIZE_QCIF:     return "QCIF(176x144)";
    case FRAMESIZE_HQVGA:    return "HQVGA(240x176)";
    case FRAMESIZE_240X240:  return "240x240";
    case FRAMESIZE_QVGA:     return "QVGA(320x240)";
    case FRAMESIZE_320X320:  return "320x320";
    case FRAMESIZE_CIF:      return "CIF(400x296)";
    case FRAMESIZE_HVGA:     return "HVGA(480x320)";
    case FRAMESIZE_VGA:      return "VGA(640x480)";
    case FRAMESIZE_SVGA:     return "SVGA(800x600)";
    case FRAMESIZE_XGA:      return "XGA(1024x768)";
    case FRAMESIZE_HD:       return "HD(1280x720)";
    case FRAMESIZE_SXGA:     return "SXGA(1280x1024)";
    case FRAMESIZE_UXGA:     return "UXGA(1600x1200)";
    case FRAMESIZE_FHD:      return "FHD(1920x1080)";
    case FRAMESIZE_P_HD:     return "P_HD(720x1280)";
    case FRAMESIZE_P_3MP:    return "P_3MP(864x1536)";
    case FRAMESIZE_QXGA:     return "QXGA(2048x1536)";
    case FRAMESIZE_QHD:      return "QHD(2560x1440)";
    case FRAMESIZE_WQXGA:    return "WQXGA(2560x1600)";
    case FRAMESIZE_P_FHD:    return "P_FHD(1080x1920)";
    case FRAMESIZE_QSXGA:    return "QSXGA(2560x1920)";
    case FRAMESIZE_5MP:      return "5MP(2592x1944)";
    default:                 return "?";
    }
}

/* 各 framesize 的默认 JPEG 质量：分辨率越高给越好的质量，避免糊 */
static int default_quality_for(int fs)
{
    if (fs >= FRAMESIZE_QSXGA) {
        return 10;
    }
    if (fs >= FRAMESIZE_UXGA) {
        return 12;
    }
    if (fs >= FRAMESIZE_XGA) {
        return 12;
    }
    if (fs >= FRAMESIZE_SVGA) {
        return 12;
    }
    return 14;
}

esp_err_t camera_hal_flash(bool on)
{
#if PIN_FLASH >= 0
    static bool cfg_done;
    if (!cfg_done) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << PIN_FLASH,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&io);
        if (e != ESP_OK) {
            return e;
        }
        gpio_set_level(PIN_FLASH, 0);
        cfg_done = true;
    }
    gpio_set_level(PIN_FLASH, on ? 1 : 0);
    return ESP_OK;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ================================ init ================================ */
esp_err_t camera_hal_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* --- framesize / quality：**优先用运行期配置**（/config.json），Kconfig 只作兜底 ---
     *
     * ★ 为什么必须在 init 这里就定下来，而不是 init 之后再 set_param 去改：
     *   esp32-camera 的 DMA/framebuffer 是**按 init 时的 framesize 分配**的，
     *   事后改大分辨率缓冲区就不够用（画面截断/花屏，而且很难看出是配置的锅）。
     *   在这里定，两个方向都安全：改大缓冲区跟着大，改小也不浪费 PSRAM。
     *
     * 0.1.8 之前这里只读 CONFIG_SNAP_DEFAULT_FRAMESIZE，而 /config.json 里的
     * frame_size / jpeg_quality **从来没进过传感器**（`camera_hal_set_param()`
     * 当时全工程没有调用点）：网页上把分辨率从 VGA 改成 QVGA，抓拍耗时和体积纹丝不动，
     * 而 /api/status 看上去"改成功了" —— 典型"配置骗人"故障。 */
    config_store_t *rcfg = config_store_get();
    int fs = (rcfg && rcfg->frame_size > 0) ? rcfg->frame_size : CONFIG_SNAP_DEFAULT_FRAMESIZE;
    if (rcfg && rcfg->frame_size > 0 && rcfg->frame_size != CONFIG_SNAP_DEFAULT_FRAMESIZE) {
        LOGI(TAG, "framesize 取自运行期配置 =%d（Kconfig 默认 %d）",
             rcfg->frame_size, CONFIG_SNAP_DEFAULT_FRAMESIZE);
    }
    if (fs > SENSOR_MAX_FS) {
        LOGW(TAG, "framesize=%d 超出 %s 上限 %d -> clamp", fs, SENSOR_NAME, SENSOR_MAX_FS);
        fs = SENSOR_MAX_FS;
    }
#if defined(CONFIG_CAM_OV5640)
    /* 5MP 的额外风险提示 + 默认降到 XGA */
    if (fs > FRAMESIZE_XGA) {
        LOGW(TAG, "5MP single frame + PSRAM queue risk: framesize=%d(%s) -> clamp to XGA",
             fs, camera_hal_framesize_name(fs));
        fs = FRAMESIZE_XGA;
    }
#endif
    if (fs < FRAMESIZE_QVGA) {
        LOGW(TAG, "framesize=%d 过小，抬到 QVGA", fs);
        fs = FRAMESIZE_QVGA;
    }
    s_framesize      = fs;
    s_framesize_init = fs;    /* framebuffer 就是按这个尺寸分配的，改大必须先重启 */
    /* 质量：优先用运行期配置；配置没给有效值（或还没加载配置）时按**最终**的
     * framesize 取默认值 —— 所以这个赋值必须在上面所有 clamp 之后。 */
    if (rcfg && rcfg->jpeg_quality >= 4 && rcfg->jpeg_quality <= 63) {
        s_quality = rcfg->jpeg_quality;
    } else {
        s_quality = default_quality_for(fs);
    }

    const int xclk_mhz = CONFIG_SNAP_XCLK_MHZ;
    snprintf(s_pid_str, sizeof(s_pid_str), "0x%04X", (unsigned)SENSOR_PID);

    /* --- 组装 sensor 配置：只填当前这一款，其余字段为 -1 --- */
    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA,
        .pin_sccb_scl = CAM_PIN_SCL,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = xclk_mhz * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,       /* 任务书 5.2 固定 JPEG */
        .frame_size   = (framesize_t)fs,
        .jpeg_quality = s_quality,
        .fb_count     = 2,                    /* 双缓冲 */
        .fb_location  = CAMERA_FB_IN_PSRAM,
        /* ★ 用 WHEN_EMPTY（不是 LATEST），但**别把它理解成"按需取帧"**（本文件旧注释
         *   就是这么写的，是错的，已更正）：
         *   · LATEST 的队列长度是 `frame_cnt-1`，cam_task 会**一直**采帧、满了挤掉最旧的
         *     ⇒ 相机永远在跑，白耗 DMA/CPU（本工程几秒才要一帧）；
         *   · WHEN_EMPTY 的队列长度是 `frame_cnt`，**队列满了就停采**，等应用消费掉一帧
         *     再继续 —— 低频场景下自然省电省总线。
         * ★ 两者都"不是要才采"：只要队列有空位，驱动就会自己往前采帧。
         *   这正是历史上"抓到的是上一帧缓存"缺陷的根源，
         *   修法见本文件 camera_hal_grab() 上面的长注释。 */
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
#if SENSOR_HAS_AF
        .sccb_i2c_port = 0,
#endif
    };

    /* ★ 重要（已核对 esp32-camera 2.1.7 的 esp_camera.h）：
     * camera_config_t **没有** sensor 字段，也没有 OV2640_SENSOR 之类的枚举。
     * 该版本用"运行时 SCCB 探测 PID + 编译期只保留一个驱动"的方式选传感器：
     *  - 编谁的驱动由 **managed_components/espressif__esp32-camera/Kconfig** 的
     *    `CONFIG_<型号>_SUPPORT` 决定（默认全 y，会把三个驱动都链进来）；
     *  - 这里用 `idf_build_set_property` 的等价手段 —— 在 sdkconfig.defaults 里
     *    只打开当前型号的 *_SUPPORT，其余用 `# ... is not set` 关掉
     *    （见 sdkconfig.defaults 的"传感器驱动裁剪"段，与 CONFIG_CAM_OVXXXX 一一对应）。
     * 因此本工程**不需要**在 cfg 里填 sensor —— 探测是��动的，
     * 而"编译期宏切换"体现为"固件里只编进一款驱动"，符合任务书禁止运行时探测切换的要求。 */
#if defined(CONFIG_CAM_OV2640)
    /* OV2640 无 AF，sccb_i2c_port 保持默认 0 */
#elif defined(CONFIG_CAM_OV3660)
    /* OV3660 无 AF */
#elif defined(CONFIG_CAM_OV5640)
    /* OV5640 的 AF 由 CONFIG_CAMERA_AF_SUPPORT 控制（默认 n） */
#else
#error "no camera sensor selected"
#endif

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        /* 0x105 = ESP_ERR_NOT_FOUND（SCCB 无应答/PID 不符）
         * 0x106 = ESP_ERR_NOT_SUPPORTED（PID 认出来了但没编对应驱动） */
        LOGE(TAG, "init failed err=0x%x (%s) sensor=%s",
             err,
             (err == ESP_ERR_NOT_FOUND)     ? "sensor not found / SCCB no ack" :
             (err == ESP_ERR_NOT_SUPPORTED) ? "pid not compiled in" :
             (err == ESP_ERR_INVALID_SIZE)  ? "fb alloc fail / PSRAM" : "other",
             SENSOR_NAME);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        LOGE(TAG, "esp_camera_sensor_get() NULL");
        esp_camera_deinit();
        return ESP_FAIL;
    }
    s_pid = s->id.PID;

    /* ★★ 2026-09-28：传感器"软件掉电"自愈（真机踩到，代价是再也拍不了照）★★
     *
     * 背景：摄像头模组的 3.3V 是**常电**（原理图 Camera Socket 直挂 VCC3V3、
     *   路径上没有开关），所以传感器的寄存器状态**跨 ESP32 复位保留**。
     *   一旦睡前写过 0x3008 bit6=1（软件掉电），重启后 init 时 SCCB 仍能读到
     *   PID（"[CAM ] init ok" 照样打印），但**一帧都出不来**：
     *       W cam_hal: Failed to get frame: timeout
     *       [CAM W] grab fail (fb NULL)
     *
     * 为什么"只清 bit6"没用：掉电期间 esp_camera_init() 写进去的时序/PLL/格式
     *   寄存器**并未生效**（传感器在掉电态不接收配置）。所以必须：
     *     ① 先写回 0x3008=0x02 退出掉电；
     *     ② 再 deinit + 重新 init —— 把整套配置在"醒着的"传感器上重放一遍。
     *
     * ★ 只在 bit6 真为 1 时才做：重新 init 要 1~1.5s，60 秒唤醒一次的电池设备
     *   不能每次开机都付这笔账。get_reg 是 esp32-camera 的标准接口
     *   （OV5640 读 AF 状态 0x3029 用的就是它）。
     */
    /* ★ 2026-09-28 调试：**暂时改成无条件**做一次"唤醒 + 重新 init"。
     *
     *  为什么：条件版（读 0x3008 bit6）在真机上**没有触发** —— 日志里看不到
     *  "sensor 处于软件掉电态"那行；而 0x3660 的 PID 明明能读到、却一帧不出，
     *  仍是"传感器配置丢失"的典型表现。可能 get_reg 的返回语义与预期不同
     *  （读到 0 到底是"bit6=0"还是"读失败"，无法区分）。
     *  所以先用无条件版把方向证实；确认有效后把下面的 1 改回 0 即可恢复条件版。 */
#define CAM_WAKE_UNCOND  0
#if CAM_WAKE_UNCOND
    if (s->set_reg) {
        LOGW(TAG, "无条件唤醒传感器并重新 init（调试开关 CAM_WAKE_UNCOND=1）");
#else
    if (s->get_reg && s->set_reg && s->get_reg(s, 0x3008, 0x40) != 0) {
        LOGW(TAG, "sensor 处于软件掉电态(0x3008 bit6=1) -> 唤醒并重新 init");
#endif
        /* ★ 2026-09-28 实测补充：**单清 bit6 不够**（真机验证：清了 bit6、
         *   又 deinit+init 整套重放，仍然 "Failed to get frame: timeout"）。
         *   OV 系列的标准"复位 -> 上电"配对是：
         *       0x3008 = 0x82   （bit7=1 software reset）
         *       ↓ 等 30ms
         *       0x3008 = 0x02   （bit7/6=0，正常工作）
         *   所以这里补上软件复位这一步，再重新 init。 */
        s->set_reg(s, 0x3008, 0xFF, 0x82);      /* bit7=1 -> 软件复位 */
        vTaskDelay(pdMS_TO_TICKS(30));
        s->set_reg(s, 0x3008, 0xFF, 0x02);      /* bit7/6=0 -> 正常工作 */
        vTaskDelay(pdMS_TO_TICKS(30));
        /* ★ 补上"输出 pad 使能"：掉电可能把 VSYNC/HREF/PCLK/D0-D7 的驱动一并关掉，
         *   症状特征就是"SCCB 能读 PID、引脚一帧不动" —— 只清 0x3008 不够。
         *   0x3017/0x3018 是 OV 系列的 pad 输出使能寄存器，这里全开兜底。 */
        s->set_reg(s, 0x3017, 0xFF, 0xFF);
        s->set_reg(s, 0x3018, 0xFF, 0xFF);

        esp_camera_deinit();
        if (esp_camera_init(&cfg) != ESP_OK) {
            LOGE(TAG, "唤醒传感器后重新 init 失败 -> 拍照不可用");
            return ESP_FAIL;
        }
        s = esp_camera_sensor_get();
        if (s == NULL) {
            LOGE(TAG, "重新 init 后 sensor 句柄为 NULL");
            return ESP_FAIL;
        }
        LOGI(TAG, "sensor 已退出软件掉电并重新 init 成功");
    }

    /* PID 与编译期选择不符时报错（这是最容易被忽略的"接错摄像头"） */
    if (s_pid != SENSOR_PID) {
        LOGW(TAG, "pid mismatch! detected=0x%04X expected=0x%04X (%s)",
             (unsigned)s_pid, (unsigned)SENSOR_PID, SENSOR_NAME);
    }

    /* OV5640 支持 dzoom，不裁剪；其余保持一致 */
    s_inited = true;

    LOGI(TAG, "init ok sensor=%s pid=0x%04X frame=%s q=%d xclk=%dMHz fb=2xPSRAM",
         SENSOR_NAME, (unsigned)s_pid, camera_hal_framesize_name(fs),
         cfg.jpeg_quality, xclk_mhz);

#if SENSOR_HAS_AF
    /* OV5640：读寄存器判断 AF 是否可用（AF 版才有），仅打印不作为失败依据 */
    {
        int af = 0;
        if (s->get_reg) {
            /* 0x3029 bit2 = AF 完成后置位；这里只做"存在性"探测：
             * 读 0x3000 低位（AF 版本该寄存器可读），读失败说明不支持。 */
            af = s->get_reg(s, 0x3029, 1);
        }
        LOGI(TAG, "OV5640 AF supported=%s (reg 0x3029=%d)", (af >= 0) ? "yes" : "no", af);
    }
#endif
    return ESP_OK;
}

/* ================================ grab ================================ */

/* 最多丢弃几张旧帧再放弃（fb_count=2，所以 2~3 张足够把队列掏空）。
 * 每一轮"丢弃"都是把帧**立刻还回**驱动，保持有空槽让它继续采。 */
#define CAM_GRAB_MAX_DRAIN  4

/**
 * ★ 2026-09-27 修掉的一个**真机可见的严重缺陷**：抓到的帧是"上一帧的缓存"。
 *
 * 现场症状（用户报）：PIR 触发 / Telegram `/snap` 之后，上传的照片是**上一次
 * 触发时**的画面 —— 人已经站在镜头前了，照片里却没有他。
 *
 * 根因（读 `cam_hal.c` 的 `cam_task()` 确认，不是猜的）：
 *   · 驱动有 `frame_cnt`(=fb_count=2) 个 fb 缓冲，加一个 `frame_buffer_queue`；
 *   · 只要队列里还有空位，它**每次 VSYNC 就自己采一帧 push 到队尾**，满了才停
 *     —— 这才是 `CAMERA_GRAB_WHEN_EMPTY` 的真实语义："有空位就采"，**不是**"要才采"
 *     （本文件原先的注释写成"按需取帧"，是错的，已一并更正）；
 *   · `cam_take()` 用的是 `xQueueReceive` ⇒ 取的是**队首 = 最旧**的那一张。
 * 于是本工程"几秒才抓一帧"的用法必然踩中：
 *   上一次抓拍归还帧 → 立刻空出一个槽 → 驱动马上采一帧塞进去 →
 *   几秒后触发时 `esp_camera_fb_get()` 拿到的正是**几秒前**躺进去的那张。
 * 顺带解释了另一处一直没想通的现象：补光 300ms 与不补光的画面亮度只差 0.2%
 *   —— 因为那张帧是在**灯亮之前**就曝光完成的（见 docs/debug_notes.md §9.9）。
 *
 * 修法（不用猜阈值）：`fb->timestamp` 是驱动在**帧起始 VSYNC** 时刻打的
 *   `esp_timer_get_time()`，所以直接比较
 *
 *        fb->timestamp >= 本次调用时刻   ⇒  这帧是"请求之后才开始曝光"的
 *
 *   不满足就丢掉、再取下一张。判据与场景、分辨率、曝光时长都无关。
 *
 * 代价：正常情况下多等 ~1 个帧周期（实测 `grab` 从 0~1ms 变成几十 ms），
 *   但换来的是"触发那一刻"的画面 —— 这正是要拍的。
 */
camera_fb_t *camera_hal_grab(uint32_t timeout_ms)
{
    if (!s_inited) {
        return NULL;
    }

    const int64_t t_req = esp_timer_get_time();
    camera_fb_t *fb      = NULL;
    int          drained = 0;

    while (drained <= CAM_GRAB_MAX_DRAIN) {
        /* 队列空时会等下一帧（fb_get 内部固定 4s 超时；本函数不用它做超时判断） */
        fb = esp_camera_fb_get();
        if (fb == NULL) {
            LOGW(TAG, "grab fail (fb NULL)");
            return NULL;
        }

        /* 空帧（传感器没配好 / 曝光过长）当废帧处理，避免把 0 字节文件写进 SD */
        if (fb->len == 0 || fb->buf == NULL) {
            LOGW(TAG, "drop empty frame len=%u", (unsigned)fb->len);
            esp_camera_fb_return(fb);
            fb = NULL;
            drained++;
            continue;
        }

        const int64_t ts = (int64_t)fb->timestamp.tv_sec * 1000000 + (int64_t)fb->timestamp.tv_usec;
        if (ts >= t_req) {
            break;                          /* ★ 本次请求之后才开始曝光 -> 就是它 */
        }

        LOGW(TAG, "drop stale frame len=%u age=%lldms (早于本次请求)",
             (unsigned)fb->len, (long long)((esp_timer_get_time() - ts) / 1000));
        esp_camera_fb_return(fb);           /* 立刻还回，让驱动有空槽继续采 */
        fb = NULL;
        drained++;

        if (timeout_ms && (esp_timer_get_time() - t_req) / 1000 > (int64_t)timeout_ms) {
            LOGW(TAG, "drain 超过 %ums，接受手上一张", (unsigned)timeout_ms);
            break;
        }
    }

    if (fb == NULL) {
        /* 兜底：宁可给一张可能偏旧的，也不要整次抓拍失败（极暗场景下曝光很长时才会走到） */
        fb = esp_camera_fb_get();
        if (fb == NULL) {
            LOGW(TAG, "grab fail (fb NULL, drained=%d)", drained);
            return NULL;
        }
        LOGW(TAG, "grab accepted (可能偏旧) len=%u drained=%d",
             (unsigned)fb->len, drained);
        return fb;
    }

    const int64_t ts  = (int64_t)fb->timestamp.tv_sec * 1000000 + (int64_t)fb->timestamp.tv_usec;
    LOGI(TAG, "grab ok len=%u age=%lldms drained=%d",
         (unsigned)fb->len, (long long)((esp_timer_get_time() - ts) / 1000), drained);
    return fb;
}

esp_err_t camera_hal_return(camera_fb_t *fb)
{
    if (fb == NULL) {
        return ESP_OK;      /* 幂等 */
    }
    esp_camera_fb_return(fb);
    return ESP_OK;
}

/* ============================== set_param ============================== */
esp_err_t camera_hal_set_param(cam_param_t p)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (p.id) {
    case CAM_PARAM_FRAMESIZE: {
        int fs = p.value;
        if (fs > SENSOR_MAX_FS) {
            LOGW(TAG, "set framesize=%d 超上限 %d -> clamp", fs, SENSOR_MAX_FS);
            fs = SENSOR_MAX_FS;
        }
#if defined(CONFIG_CAM_OV5640)
        if (fs > FRAMESIZE_XGA) {
            LOGW(TAG, "5MP single frame + PSRAM queue risk -> clamp to XGA");
            fs = FRAMESIZE_XGA;
        }
#endif
        if (fs < FRAMESIZE_QVGA) {
            fs = FRAMESIZE_QVGA;
        }
        esp_err_t e = (s->set_framesize(s, (framesize_t)fs) == 0) ? ESP_OK : ESP_FAIL;
        if (e == ESP_OK) {
            s_framesize = fs;
            LOGI(TAG, "set framesize=%s", camera_hal_framesize_name(fs));
        } else {
            LOGE(TAG, "set_framesize(%d) fail", fs);
        }
        return e;
    }
    case CAM_PARAM_QUALITY: {
        int q = p.value;
        if (q < 4)  { q = 4; }
        if (q > 63) { q = 63; }
        s->set_quality(s, q);
        s_quality = q;             /* /api/status 报的是"实际生效值" */
        LOGI(TAG, "set quality=%d", q);
        return ESP_OK;
    }
    case CAM_PARAM_BRIGHTNESS:
        s->set_brightness(s, p.value);
        return ESP_OK;
    case CAM_PARAM_CONTRAST:
        s->set_contrast(s, p.value);
        return ESP_OK;
    case CAM_PARAM_SATURATION:
        s->set_saturation(s, p.value);
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* =========================== apply_runtime =========================== */
esp_err_t camera_hal_apply_runtime(int framesize, int quality)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    int fs = framesize;
    if (fs > SENSOR_MAX_FS) { fs = SENSOR_MAX_FS; }
    if (fs < FRAMESIZE_QVGA) { fs = FRAMESIZE_QVGA; }

    esp_err_t ef = ESP_OK;
    if (fs > s_framesize_init) {
        /* ★ 只拦"改大"。framebuffer 是 init 时按 framesize 分配的，
         *   事后改大缓冲区不够 -> 画面被截断/花屏，而且不会有任何报错。
         *   不在这里硬拒绝整个请求：质量还是照常应用，只是分辨率要等重启。 */
        LOGW(TAG, "framesize %s -> %s 属于改大：缓冲区按 init 时的 %s 分配，"
                  "**本次不生效，请重启设备**（重启后 init 会直接用新值）",
             camera_hal_framesize_name(s_framesize),
             camera_hal_framesize_name(fs),
             camera_hal_framesize_name(s_framesize_init));
    } else if (fs != s_framesize) {
        ef = camera_hal_set_param((cam_param_t){ .id = CAM_PARAM_FRAMESIZE, .value = fs });
    }

    esp_err_t eq = camera_hal_set_param((cam_param_t){ .id = CAM_PARAM_QUALITY, .value = quality });
    return (ef == ESP_OK && eq == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t camera_hal_set_flicker(int hz)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ esp32-camera 2.1.7 的 sensor_t **没有 set_flicker** 成员
     * （已核对该版本 driver/include/sensor.h 的完整函数指针表：
     *  set_brightness/contrast/saturation/…/set_vflip/hmirror/dcw/…）。
     *  anti-flicker 只能用 menuconfig 的 CONFIG_CAMERA_FLICKER 或写寄存器。
     * 这里如实返回"不支持"，不假装成功 —— 上层与 /api/config 会看到这个结果。 */
    LOGW(TAG, "set_flicker(%d) not supported by esp32-camera 2.1.7 sensor_t", hz);
    (void)s;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ============================== 只读访问 ============================== */
const char *camera_hal_sensor_name(void)
{
    return SENSOR_NAME;
}

const char *camera_hal_expected_pid_str(void)
{
    return s_pid_str;
}

uint16_t camera_hal_pid(void)
{
    return s_pid;
}

int camera_hal_current_framesize(void)
{
    return s_framesize;
}

int camera_hal_current_quality(void)
{
    return s_quality;
}
