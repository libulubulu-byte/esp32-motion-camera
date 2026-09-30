/**
 * @file snapshot_pipeline.c
 * @brief 拍照管道：补光 -> grab -> (SD 落盘 | PSRAM 拷贝) -> 归还 -> 入队
 *
 * framebuffer 生命周期（任务书 0.10：每个 fb 有唯一归还/释放路径）：
 *
 *   fb = camera_hal_grab()
 *     ├─ NULL              -> 直接 return false（不需要归还）
 *     ├─ SD 可用           -> 写 jpg + meta -> camera_hal_return(fb)  [路径 A]
 *     ├─ SD 不可用         -> malloc+memcpy -> camera_hal_return(fb)  [路径 B]
 *     └─ 拷贝失败          -> camera_hal_return(fb)                   [路径 C]
 *
 *   三条路径都在函数末尾**唯一出口**处归还，不会漏。
 *   另外为了 /api/last_snapshot 降级可用，路径 B 额外保留一份最新 JPEG
 *   的独立拷贝（s_last_raw），它由本模块自己管理，与队列项互不相干。
 */

#include "snapshot_pipeline.h"
#include "buzzer_hal.h"   /* ★ PIR 触发且判定有人时鸣叫"滴滴" */
#include "sleep_mgr.h"    /* ★ 判定无人 -> 本次唤醒没活干了，回睡 */
#include "camera_hal.h"
#include "config_store.h"
#include "logger.h"
#include "sd_hal.h"
#include "upload_queue.h"
#include "wifi_hal.h"
#include "time_sync.h"
#if CONFIG_SNAP_ENABLE_HUMAN_STUB
#include "human_stub.h"
#endif
#if CONFIG_SNAP_ENABLE_HUMAN_DETECT
#include "human_detect.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "sdkconfig.h"

static const char *TAG = LOG_T_PIPE;

/* ---- 统计 ---- */
static uint32_t s_ok;
static uint32_t s_fail;

/* 人形门控统计（调试"是不是人才上传"用，见 snapshot_pipeline.h 的语义说明） */
static uint32_t s_human_run;
static uint32_t s_human_hit;
static uint32_t s_human_miss;

/* ---- 降级时"最近一张"的独立保留副本（PSRAM） ---- */
#define LAST_RAW_MAX  (2 * 1024 * 1024)
static uint8_t *s_last_raw;
static size_t   s_last_raw_len;
static char     s_last_path[200];

/* 累计 memory 水位，供验收项"连拍 50 次 psram_after 无持续下降"对比 */
static uint32_t s_psram_min;

esp_err_t snapshot_pipeline_init(void)
{
    s_psram_min = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    LOGI(TAG, "init ok mode=%s last_raw_cap=%uKB psram=%u",
         upload_queue_mode_str(), LAST_RAW_MAX / 1024, (unsigned)s_psram_min);
    return ESP_OK;
}

uint32_t snapshot_pipeline_ok_count(void)
{
    return s_ok;
}

uint32_t snapshot_pipeline_fail_count(void)
{
    return s_fail;
}

uint32_t snapshot_pipeline_psram_min(void)
{
    return s_psram_min;
}

uint32_t snapshot_pipeline_human_run_count(void)  { return s_human_run;  }
uint32_t snapshot_pipeline_human_hit_count(void)  { return s_human_hit;  }
uint32_t snapshot_pipeline_human_miss_count(void) { return s_human_miss; }

/* 保留一份"最近一张"给降级模式下的 /api/last_snapshot。
 * ★ 这里必须用独立缓冲，不能复用队列项的 buf —— 队列项随时会被消费和释放。 */
static void keep_last_raw(const uint8_t *jpg, size_t len, const char *rel_path)
{
    if (len == 0 || len > LAST_RAW_MAX) {
        return;
    }
    uint8_t *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!p) {
        LOGW(TAG, "last_raw alloc fail len=%u -> /api/last_snapshot 降级时不可用",
             (unsigned)len);
        return;
    }
    memcpy(p, jpg, len);
    if (s_last_raw) {
        free(s_last_raw);      /* 先分配后释放：避免瞬时双倍占用 */
    }
    s_last_raw = p;
    s_last_raw_len = len;
    strlcpy(s_last_path, rel_path ? rel_path : "", sizeof(s_last_path));
}

/* ------------------------------------------------------------------ */
/* 坏帧判据（"糊了 / 是空帧"）—— 判定逻辑与理由见下面 grab 段的长注释。
 *
 * 只用 **JPEG 体积**这一个免费信号：它是"画面信息量"的廉价代理 ——
 * 空帧 / 整片纯色 / 糊掉的帧都会明显偏小。
 * 实测（本工程 SVGA + JPEG q12）：正常实景 10.7~13.9KB，整片灰 ≈ 7KB。
 *
 * 为什么不用真清晰度算法（解码后算梯度方差）：
 * 工程里现成的解码器（esp_jpeg / TJpgDec）全尺寸解码 ≈ 50ms，
 * 比"补拍一张"本身还贵，而且阈值随场景漂移、更难标定。
 */
#define SNAP_BAD_LEN_FLOOR   4096   /* **还没有基准时**的绝对下限（挡住整片纯黑/空帧） */

/* 低于基准值的百分之几，就认为"整片糊 / 灰 / 空" -> 立刻补拍一张。
 *
 * ★ 2026-09-27 真机标定（两次遮挡实测）后的**结论**：
 *   遮挡帧实测落在正常帧的 **19% ~ 67%**（手掌挡 7436B、手指贴 11575B，
 *   对照正常 17~38KB，VGA + q12 + 300ms 补光）——
 *   比"整片糊"直观想象的大得多：JPEG 体积此时由**噪声与渐变**主导，不是细节。
 *   而"正常但简单"的画面同样能到 73%（17233 → 12591，有人走过）。
 *   ⇒ **67% 与 73% 已经重叠，体积判据分不开"糊"和"场景本身简单"**。
 *   所以要"抓住整片糊"，线只能压到 **75**；代价是正常场景波动也会触发补拍
 *   （实测每次只多 ~1~2ms，且最终取更大的那张，代价可接受）。
 *   想真正区分，必须解码看内容（见 grab 段注释里的"更好的判据"与 §9.9 待办）。
 *   对照档位：35/60 = 抓不到本次实测的遮挡帧（已验证 retake=0）；99 = 近乎每次都补。
 * 注意它与下面的下限无关：下限只在"还没有基准"时才生效。 */
#define SNAP_BAD_LEN_PCT     75

/* 最近一张**合格帧**的体积，作为相对判据的基准。
 *
 * 为什么必须是"相对"的：固定字节阈值分不清"画面糊"和"场景本身简单"
 * （对着一面纯白墙，正常帧本来也只有几 KB）—— 这正是 0.1.5 设计时**否决**
 * "空帧守卫"方案的理由，改成与最近一张合格帧比较后，这个理由才不再成立。 */
static uint32_t s_ref_len;

/* 建立 s_ref_len 那一帧的"模式"（framesize + jpeg_quality）。
 * 这两者一变，同样场景的字节数就差一个数量级（QVGA vs VGA 差 ~4 倍，
 * q63 vs q12 差 ~25%），沿用旧基准只会白白补拍十几次 —— 所以发现不一致就立即重置。
 * 初始 -1 表示"还没建立过"，此时不打印重置日志。 */
static int  s_ref_framesize = -1;
static int  s_ref_quality   = -1;

/* false = 基准还没建立（刚开机 / 刚换分辨率画质），**下一个样本要丢掉**：
 * 换模式后的第一帧常常是瞬态垃圾（实测：切到 QVGA 后第一帧 56292B，
 * 稳定后只有 ~4164B）。拿它当基准，后面十几次抓拍都会被误判成坏帧。 */
static bool s_ref_ready;

static uint32_t bad_len_threshold(void)
{
    if (s_ref_len == 0) {
        return SNAP_BAD_LEN_FLOOR;
    }
    /* ★ 有基准时**只用相对判据**，不再与 SNAP_BAD_LEN_FLOOR 取大。
     *   取大过会害了小画面：QVGA(320x240) + 高压缩时正常帧只有 ~3KB，
     *   若下限 4096 一直压着，就会**每次抓拍都被判成坏帧、每张都白补一次**。
     *   下限只该在"还没有基准"的那一两张里起作用。 */
    return (uint32_t)((uint64_t)s_ref_len * SNAP_BAD_LEN_PCT / 100);
}

/* 用最终**保留**下来的那一帧更新基准：
 *   · 坏帧   -> 只让它缓慢下移（7/8）：既防止"连续几张坏帧把基准拖到再也测不出来"，
 *               又让"环境确实一直很暗"这种情况最终能重新定基线
 *   · 合格帧 -> 允许跟，但**限制单次上跳幅度**（最多 1.5 倍），见下面注释 */
static void ref_len_update(uint32_t len, uint32_t thresh)
{
    if (len < thresh) {
        s_ref_len = (s_ref_len * 7 + len) / 8;
        return;
    }

    /* 合格帧：不直接跟，而是限制单次上跳。
     *
     * 为什么：实测踩到过"大而坏"的帧 —— 运行时把分辨率改成 QVGA 后的第一帧是
     * **56292B**（正常 QVGA 帧的 ~14 倍，传感器刚切模式时的瞬态产物），
     * 它体积够大、被当成合格帧。基准一旦被顶到 5.6 万，之后每一次抓拍都会
     * 被判成坏帧白补一张，且按 7/8 回落要几十次才收敛（PIR 场景下就是几个小时）。
     * 限制 1.5 倍/次后：正常的场景变复杂（几次抓拍就到）依然跟得上，
     * 单个离群帧的影响被压到 1.5 倍以内。
     * （注意 s_ref_len==0 是开机第一张，必须直接赋值，否则 1.5×0 永远长不大。） */
    if (s_ref_len == 0 || len <= (s_ref_len / 2) * 3) {
        s_ref_len = len;
    } else {
        s_ref_len = (s_ref_len / 2) * 3;
    }
}

/* ------------------------------------------------------------------ */
/* 人形门控                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 本次触发**是否需要**过人形门控
 *
 * ★ 规则（2026-09-28 用户明确要求："远程命令不用判断是不是人形，
 *   只有本地才会判断"）：
 *
 *   **本地自动触发** → 判人形
 *     PIR / 按键 / 定时抓拍都是"屋里可能有、也可能没有人"的被动触发，
 *     判一下才能避免窗帘、树影、宠物、路人引起的无意义上传 ——
 *     这正是人形门控"省流量 + 不打扰"的全部意义。
 *
 *   **远程命令** → **不判**，拍什么就上报什么
 *     Telegram /snap、POST /api/snapshot、MQTT 命令都是**人主动**要一张。
 *     若这些也判，人站在镜头前发 /snap 却因为"模型这一帧没检出"而收不到
 *     照片，现场只会被理解成"设备坏了"——远程命令的语义就是"我要一张，
 *     别替我做决定"。
 *
 * ★ 不判人形 ≠ 走另一条链路：抓拍、判坏补拍、入队、上报、计数完全一致，
 *   只是**省掉一次推理**（约 100~130ms，反而更快）。
 *
 * ★ 想调整策略只改这一个函数（主帧与补拍帧都走它），不要在两处调用点各判一次。
 */
static bool src_needs_gate(trig_src_t src)
{
    switch (src) {
    case TRIG_SRC_PIR:
    case TRIG_SRC_BUTTON:
    case TRIG_SRC_TIMER:
        return true;                    /* 本地自动触发 -> 判人形 */

    case TRIG_SRC_TELEGRAM:
    case TRIG_SRC_REMOTE_HTTP:
    case TRIG_SRC_REMOTE_MQTT:
        return false;                   /* 远程命令 -> 不判 */

    default:
        /* 未知来源按最保守处理：宁可多一次推理，也不要悄悄放行 */
        return true;
    }
}

/**
 * @brief 对一帧跑人形检测，并回答"这张要不要上传"
 *
 * ★ 为什么抽成函数而不是内联在 capture 里：**判坏补拍也要过门控**。
 *   否则会出现一个很隐蔽的漏洞 —— 第一帧判为"无人"、补拍帧不再检查就上传，
 *   于是"不是人不上传"在补拍路径上直接失效（夜间 PIR 触发时这条路径很常见）。
 *
 * @return true = 允许上传（检出人脸，或检测本身失败走 fail-open）
 * @note   开关关闭时本函数恒返回 true，照片照常上传，不做任何黑箱丢弃。
 */
static bool human_gate_take(const camera_fb_t *fb, trig_src_t src, const char *phase)
{
#if CONFIG_SNAP_ENABLE_HUMAN_DETECT
    human_detect_result_t hd = {0};
    bool is_human = human_detect_run(fb->buf, fb->len, &hd);

    s_human_run++;

    /* fail-open 的判据：检测报错时 human_detect 内部会把 human 置 true，
     * 且它的 err 计数会增加 —— 这里用"结果说是人但一个框都没有"来识别，
     * 是为了让日志能区分"真检出人脸"和"检测坏了放行"，不然后者会被静默计入 hit。 */
    if (is_human) {
        s_human_hit++;
    } else {
        s_human_miss++;
    }

    if (!is_human) {
        /* 日志必须说清"为什么丢"，否则现场无从判断是模型错还是真没人 */
        LOGI(TAG, "非人形帧丢弃 src=%s phase=%s faces=0 score=%d decode=%dms infer=%dms len=%u",
             trig_src_str(src), phase, hd.score, hd.decode_ms, hd.infer_ms,
             (unsigned)fb->len);
    } else {
        LOGI(TAG, "人形确认 src=%s phase=%s faces=%d score=%d (decode=%dms infer=%dms)",
             trig_src_str(src), phase, hd.num_faces, hd.score, hd.decode_ms, hd.infer_ms);
    }

    return is_human;
#else
    (void)fb;
    (void)src;
    (void)phase;
    return true;        /* 门控未编译：不做任何丢弃 */
#endif
}

bool snapshot_pipeline_capture(trig_src_t src)
{
    config_store_t *cfg = config_store_get();
    sd_state_t sd = sd_hal_state();
    bool sd_ok = (sd == SD_OK || sd == SD_LOWSPACE);

    camera_fb_t *fb = NULL;
    uint8_t     *copy = NULL;       /* 只有"已移交队列"之后才不为 NULL */
    bool         queued = false;
    /* 本次是否为"人形门控判定无人 -> 策略性丢弃"。
     * 它是"本次唤醒已无事可做"的信号，用于收尾后回睡。 */
    bool         discarded_nonhuman = false;
    /* 本次是否需要过人形门控（远程命令不需要，见 src_needs_gate 的说明）。
     * ★ 只算一次：主帧与后面可能的补拍帧必须用**同一个策略**，
     *   否则会出现"主帧不判、补拍帧判"这种自相矛盾的组合。 */
    const bool   need_gate = src_needs_gate(src);

    /* ★★ 抓拍前"软件唤醒"传感器（2026-09-28 实测必需）★★
     *
     * 现场结论：OV3660 的寄存器 0x3008 bit6 一旦被置 1（软件掉电），
     *   **重启也不会自动恢复** —— 表现就是"再也拍不了照"；
     *   把 sleep_mgr 里那条睡前掉电代码退掉也没用，因为传感器已经被关死了。
     *
     * 所以每次抓拍前先写回 0x02（正常工作）。代价是 SCCB 两个字节，可忽略；
     *   换来的是"不管上一次是怎么睡的，这一次一定有图"。
     *
     * 注：sleep_mgr 里的睡前掉电目前用 #if 0 关着；将来要重新启用省电方案，
     *   **这一行就是它的必备配套** —— 否则一睡就再也醒不过来。 */
    /* ★★ 2026-09-29 抓拍前传感器体检 ★★
     *
     * 目的：把"拍不了照"一次分清是哪一类问题，别再靠猜。
     *   · PID   = 0x3660      -> SCCB(I2C) 通、传感器认到了（硬件连着的）
     *   · PID   = 0xFF/0x00/0xFFFF -> SCCB 不通 -> 排线/供电/I2C 问题
     *   · 0x3008 bit6=1       -> 还停在**软件掉电**态（需要断电或复位才能救）
     *   · 0x3017/0x3018 不是全 1 -> **输出 pad 被关**：PCLK/VSYNC/HREF/D0-D7
     *                              不驱动 -> "能读 PID、一帧不出"就是这个
     *
     * 典型判读：
     *   PID 正常 + 0x3008 正常 + pad 正常 + 但 grab 仍 timeout
     *       => 传感器配置看着都对却不出数据 -> 指向**排线接触/数据脚损伤**（硬件）
     *   PID 读不到 -> I2C/供电问题（也是硬件）
     *   0x3008 bit6=1 -> 软件掉电没救回来（本板只能断电恢复）
     */
    {
        sensor_t *sn = esp_camera_sensor_get();
        if (sn && sn->get_reg) {
            int pid_h = sn->get_reg(sn, 0x300A, 0xFF);
            int pid_l = sn->get_reg(sn, 0x300B, 0xFF);
            int sysc  = sn->get_reg(sn, 0x3008, 0xFF);
            int pad17 = sn->get_reg(sn, 0x3017, 0xFF);
            int pad18 = sn->get_reg(sn, 0x3018, 0xFF);
            LOGI(TAG, "DIAG sensor PID=0x%02X%02X 0x3008=0x%02X 0x3017=0x%02X 0x3018=0x%02X%s",
                 pid_h, pid_l, sysc, pad17, pad18,
                 (sysc & 0x40) ? " [仍处于软件掉电!]" : "");
        } else {
            LOGW(TAG, "DIAG 拿不到 sensor 句柄（get_reg 不可用）");
        }
    }

    /* 设备侧耗时打点：done: 处打 "capture took=..ms flash=..ms grab=..ms"。
     * 这是**不含网络/上传**的纯抓拍耗时，用来回答"拍一张要多久"，别拿 PC 侧
     * 的 HTTP 轮询时间当答案（每次 curl 就要 50~80ms，误差比本身还大）。 */
    int64_t t_all0  = esp_timer_get_time() / 1000;
    int64_t t_grab0 = 0;
    int64_t t_grab1 = 0;

    /* ---------- 1. 补光 ---------- */
    int flash_ms = cfg->flash_before_ms;
    bool flash_used = (flash_ms > 0);
    if (flash_used) {
        camera_hal_flash(true);
        vTaskDelay(pdMS_TO_TICKS(flash_ms));
    }

    /* ---------- 2. grab：单拍；判为坏帧则**立即补拍一张** ---------- */
    /* --- 2a. 分辨率/画质变了就立刻重置体积基准 --- */
    /* 自己发现、自己重置，不依赖上层通知：判据本来就是"与最近一张合格帧比体积"，
     * 而 VGA 换成 QVGA 后字节数差 ~4 倍，旧基准对新模式毫无意义。 */
    {
        int fs_now = camera_hal_current_framesize();
        int q_now  = camera_hal_current_quality();
        if (fs_now != s_ref_framesize || q_now != s_ref_quality) {
            if (s_ref_framesize >= 0) {
                LOGI(TAG, "模式变化 framesize/q %d/%d -> %d/%d：体积基准 %u 作废，重新标定",
                     s_ref_framesize, s_ref_quality, fs_now, q_now,
                     (unsigned)s_ref_len);
                /* ★ 顺手把驱动里**旧模式残留的帧**冲掉：fb_count=2，所以最多 2 张。
                 *   实测就是它们在作祟 —— 切到 QVGA 后头两次抓到的仍是 VGA 尺寸的老帧，
                 *   把新基准带到另一个数量级（实测基准成了 17233），结果真正的 QVGA 帧
                 *   (6066) 反被判成坏帧、白补一张。冲掉 2 张后第一帧就是新模式的了。
                 *   代价：只在换模式那一次多花 2 次抓帧（~2ms）。 */
                for (int i = 0; i < 2; i++) {
                    camera_fb_t *stale = camera_hal_grab(200);
                    if (!stale) {
                        break;
                    }
                    camera_hal_return(stale);
                }
            }
            s_ref_framesize = fs_now;
            s_ref_quality   = q_now;
            s_ref_len       = 0;
            s_ref_ready     = false;
        }
    }

    uint32_t psram_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int      retake    = 0;         /* 本次是否补拍过（0/1），进日志与耗时行 */
    uint32_t len_first = 0;         /* 第一张的体积，补拍日志里要对比 */
    LOGI(TAG, "grab start flash=%dms retake=%s ref=%u",
         flash_ms, cfg->retake_on_bad ? "on" : "off", (unsigned)s_ref_len);
    t_grab0 = esp_timer_get_time() / 1000;
    fb = camera_hal_grab(2000);

    /* ★ 判坏补拍（2026-09-27 取代 0.1.5 的"连拍 3 帧取最大"）。
     *
     * 为什么不连拍：每多抓一帧 = +1 个传感器帧周期（~20ms），而绝大多数帧都是好的
     *   —— 为少数坏帧让**每一次**抓拍都付这个代价不划算。现在是"先拍一张，
     *   判为坏帧才补拍一张"：正常情况零额外开销，坏帧时也比连抓 3 帧省。
     *
     * ⚠️ 它救不了什么，先说清楚：**运动模糊救不了**。同一次触发的第二张，
     *   场景 / 曝光时间 / 运动速度都没变，拍出来一样糊。
     *   补拍真正能救的是**瞬态坏帧**：AE/AWB 还没收敛、驱动首帧、整片纯色 /
     *   全黑、偶发撕裂帧。要"夜里走动的人不糊"，该动的是补光与曝光，不是补拍。
     *
     * 判据故意**偏敏感**（低于基准 60% 就补拍）：这里的代价是**不对称**的 ——
     *   误判只多花 ~20ms，且最终取两帧里**更大**的那张（不会更差）；
     *   漏判才会把糊图真的发出去。
     *
     * 帧所有权（本文件最硬的约束，别破坏）：
     *   全程**最多只持有 2 帧**（原帧 + 补拍帧），比较完立刻归还落败方。
     *   fb_count=2 的驱动缓冲池因此不会被占死；最终那一帧仍交给下面原有的
     *   单帧流程（唯一归还点仍在 done:），不要新增归还点。 */
    if (fb) {
        uint32_t thresh = bad_len_threshold();
        len_first = (uint32_t)fb->len;

        if (cfg->retake_on_bad && fb->len < thresh) {
            LOGW(TAG, "bad frame len=%u < %u -> retake (1/1)",
                 (unsigned)fb->len, (unsigned)thresh);
            camera_fb_t *f2 = camera_hal_grab(2000);
            if (f2) {
                uint32_t len2 = (uint32_t)f2->len;
                retake = 1;
                /* ★★ 补拍帧必须先过**同一道**人形门控，再决定留谁。
                 *
                 * 不这么做的漏洞很隐蔽但后果严重：第一帧判"无人"、补拍帧
                 * "更大所以被选中"、然后**不再检查就上传** —— "不是人不上传"
                 * 在补拍路径上直接失效。而 PIR 在夜间（画面糊、体积小）
                 * 恰恰是最容易触发补拍的场景。
                 *
                 * 注意顺序：**先检测再比大小**。若反成"先比大小、只对胜者检测"，
                 * 那张被丢弃的帧就白做了一次检测（~100ms）。 */
                /* ★ 补拍帧必须与主帧用**同一个**策略（need_gate），
                 *   不能这里写死"一定要判"：远程命令本来就不判人形，
                 *   补拍帧若反而去判，就会出现"两张策略不一致"的怪现象。 */
                bool ok2 = need_gate ? human_gate_take(f2, src, "retake") : true;

                if (len2 >= fb->len) {
                    if (ok2) {
                        camera_hal_return(fb);  /* 补拍的更大且有人 -> 换掉原来那张 */
                        fb = f2;
                    } else {
                        /* 补拍帧没人：它更大也不能用，直接丢掉它，
                         * 让原帧去走它自己的门控判定（原帧可能有人）。 */
                        camera_hal_return(f2);
                        LOGI(TAG, "补拍帧判为非人形 -> 弃用补拍帧，保留原帧");
                    }
                } else {
                    if (!ok2) {
                        /* 补拍帧没人且更小 -> 两帧都没用，明确告知而不是留一张凑数。
                         * ★ 这里**不能**直接 goto done：原帧还没经过门控，
                         *   而原帧完全可能检出人脸（两帧是不同时刻的画面）。 */
                        LOGI(TAG, "补拍帧判为非人形且更小 -> 仅保留原帧待判");
                    }
                    camera_hal_return(f2);      /* 补拍更小 -> 留原来那张 */
                }
                LOGI(TAG, "retake len=%u (first=%u) -> keep %u",
                     (unsigned)len2, (unsigned)len_first, (unsigned)fb->len);
            } else {
                LOGW(TAG, "retake grab failed -> keep first len=%u",
                     (unsigned)len_first);
            }
        }
        /* 只把最终**留下**的那帧计入基准；阈值取补拍前算好的那个，
         * 这样"补拍"这个动作本身不会改变判据口径。 */
        if (s_ref_ready) {
            ref_len_update((uint32_t)fb->len, thresh);
        } else {
            /* 基准还没建立（开机第一张 / 刚换分辨率画质）：丢掉这个样本，
             * 从下一帧起才定基准 —— 换模式后的第一帧常是"大而坏"的瞬态帧。 */
            LOGI(TAG, "体积基准未建立：丢弃本帧样本 len=%u（下一帧起生效）",
                 (unsigned)fb->len);
            s_ref_ready = true;
        }
    }
    t_grab1 = esp_timer_get_time() / 1000;

    /* 无论结果如何，补光必须立刻关（否则 OV5640 长曝光会过曝 / 灯板发热） */
    if (flash_used) {
        camera_hal_flash(false);
    }

    if (fb == NULL) {
        LOGE(TAG, "grab failed (fb NULL)");
        s_fail++;
        goto done;
    }

    LOGI(TAG, "grab ok len=%u fb=%p psram_after=%u",
         (unsigned)fb->len, (void *)fb, (unsigned)psram_before);

#if CONFIG_SNAP_ENABLE_HUMAN_STUB
    {
        int score = 0;
        human_stub_detect(fb->buf, fb->len, &score);
    }
#endif

    /* ---------- 2.5 人形门控：不是人就不上传 ----------
     *
     * 位置选择：**必须在 fb 还活着、且还没写卡/拷贝之前**。
     *   · 放在 grab 之后：这时才有像素可检；
     *   · 放在分流之前：判为"没人"就一次卡写、一次 PSRAM 拷贝都省掉，
     *     而且不会往上传队列里塞垃圾 —— 这正是本功能省流量的全部意义所在。
     *
     * 耗时：JPEG 解码 ~30ms + 两级推理 ~70ms（VGA 实测），全程阻塞本任务。
     *   调用者是 trigger 任务（6144 字节栈足够），且此时已过冷却判断，
     *   不会与另一次抓拍并发 —— human_detect 内部也就不必保护推理。
     *
     * 门控放在这里只做一次判定；真正"要不要这张"的决定在 human_gate_take 里。
     */
    /* ★ 远程命令（Telegram /snap、POST /api/snapshot、MQTT）**不判人形**：
     *   那是人主动要一张，拍什么都要给；人形门控只服务于本地自动触发。
     *   显式打一行日志 —— 否则"/snap 收到了但日志里没人形判定的记录"
     *   很容易被当成门控坏了。 */
    if (!need_gate) {
        LOGI(TAG, "远程命令 src=%s -> 跳过人形门控，本帧直接入队", trig_src_str(src));
    }

    if (need_gate && !human_gate_take(fb, src, "primary")) {
        /* ★ 2026-09-28 取证：判为"非人形"的帧同样留一份副本 ★
         *
         * 为什么必须留：门控的设计是"有脸才上传"，所以一旦检测恒判无人，
         *   **照片既不上传也不进队列** —— PC 侧拿不到任何样本，
         *   排查立刻死锁在"我看不到它到底拍到了什么"。
         *   本次现场（恒 MISS faces=0）就卡在这里，只能靠把诊断日志写进固件绕过。
         *
         * 为什么不是每次都留：只有走"被判丢弃"这条路才留，
         *   正常上传路径本来就有队列副本 / SD 文件，不必多占 PSRAM。
         *   代价是一次 memcpy（~15KB，几十微秒），换到的是"随时能取证"。
         *
         * ⚠️ 与 KEEP_NONHUMAN 的区别：那个开关是"把非人形帧**写进 SD 卡**"
         *   （占卡、目的是事后再判），这里只是**在 PSRAM 里留最近一张**
         *   （上限 LAST_RAW_MAX=2MB，后一张覆盖前一张），不落盘、不占卡。 */
        keep_last_raw(fb->buf, fb->len, NULL);
#if CONFIG_SNAP_KEEP_NONHUMAN
        /* 用户选择了"误判可回溯"：存卡但绝不入队（省流量目的不受影响） */
        if (sd_ok) {
            char keep_jpg[200] = {0}, keep_meta[200] = {0};
            sd_hal_make_paths(keep_jpg, sizeof(keep_jpg),
                              keep_meta, sizeof(keep_meta));
            if (sd_hal_write_file(keep_jpg, fb->buf, fb->len) == ESP_OK) {
                LOGI(TAG, "非人形帧已存卡 path=/%s (KEEP_NONHUMAN=y, 不上传)",
                     keep_jpg);
            } else {
                LOGW(TAG, "非人形帧存卡失败 -> 丢弃");
            }
        }
#endif
        /* ★ 不增加 s_fail：这不是"拍照失败"，是策略性丢弃。
         *   混进 fail 计数会让"抓拍成功率"这个指标失去意义。 */
        discarded_nonhuman = true;
        goto done;
    }

    /* ---------- 2b. 人形确认 -> 蜂鸣器"滴滴"（仅 PIR 触发） ----------
     *
     * ★ 2026-09-28 需求：红外(PIR)触发后，判定有人即鸣叫提示。
     *
     * 为什么放在这里：这是**门控刚判过、且 fb 还活着**的位置，语义上正好是
     *   "人形确认"的那一刻，早于落盘 / 入队 / 上传 ⇒ 提示音与照片同时发生，
     *   不会等到上传成功才响（上传还要几百 ms ~ 几秒的网络时间）。
     *
     * 只对 PIR 响：按键 / 网页 / Telegram 都是**人在操作**，再响反而吵。
     *   若要所有触发源都响，把下面的 src 判断去掉即可。
     *
     * 用图案接口而不是 beep()：beep() 是"覆盖"语义，连调两次只会响一次长的，
     *   做不出两短声。波形 80ms 响 / 120ms 停 / 再 80ms 响 = 就是"滴滴"。 */
    if (src == TRIG_SRC_PIR) {
        buzzer_hal_beep_pattern(2, 80, 120);
    }

    /* ---------- 3. meta 信息 ---------- */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    time_t now = time(NULL);
    char meta[512];
    char jpg_rel[200] = {0};
    char meta_rel[200] = {0};

    /* ---------- 4. 分流 ---------- */
    if (sd_ok) {
        /* ---- 路径 A：写卡，只把路径放进队列（不复制 JPEG，省 PSRAM） ---- */
        sd_hal_make_paths(jpg_rel, sizeof(jpg_rel), meta_rel, sizeof(meta_rel));

        esp_err_t werr = sd_hal_write_file(jpg_rel, fb->buf, fb->len);
        if (werr != ESP_OK) {
            /* 写卡失败（可能刚刚触发降级）—— 不放弃这次照片，改用内存路径 */
            LOGW(TAG, "sd write fail err=0x%x -> fallback to buffer mode", werr);
            sd_ok = false;
        } else {
            LOGI(TAG, "sd=%s path=/%s", sd_hal_state_str(sd_hal_state()), jpg_rel);
            strlcpy(s_last_path, jpg_rel, sizeof(s_last_path));
        }
    }

    if (!sd_ok) {
        /* ---- 路径 B：拷贝到 PSRAM，把所有权交给队列 ---- */
        char *mem = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        if (!mem) {
            mem = malloc(fb->len);
        }
        if (!mem) {
            LOGE(TAG, "jpeg copy alloc fail len=%u (heap=%u psram=%u)",
                 (unsigned)fb->len,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_fail++;
            goto done;
        }
        memcpy(mem, fb->buf, fb->len);
        copy = (uint8_t *)mem;
        LOGW(TAG, "queue_mode=buffer sd=%s (无卡/降级，照片驻留 PSRAM)",
             sd_hal_state_str(sd_hal_state()));

        /* 给降级模式下的 /api/last_snapshot 留一份独立副本 */
        keep_last_raw(fb->buf, fb->len, NULL);
    }

    /* ---------- 5. 组 meta 并入队 ---------- */
    meta_info_t m = {0};
    strlcpy(m.device_id, cfg->device_id, sizeof(m.device_id));
    if (time_sync_valid()) {
        m.ts = (int64_t)now;
        m.time_valid = true;
    } else {
        m.ts = 0;
        m.time_valid = false;
    }
    m.boot_ms = now_ms;
    m.trigger = src;
    m.rssi = wifi_hal_rssi();
    m.free_heap = (uint32_t)esp_get_free_heap_size();
    m.free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.sd_state = sd_hal_state_str(sd_hal_state());
    m.flash_used = flash_used ? 1 : 0;
    m.jpeg_len = fb->len;
    m.sensor = camera_hal_sensor_name();

    if (sd_ok) {
        /* 写同名 .meta.json */
        snprintf(meta, sizeof(meta),
                 "{\"device_id\":\"%s\",\"ts\":%lld,\"time_valid\":%s,\"boot_ms\":%u,"
                 "\"trigger\":\"%s\",\"rssi\":%d,\"free_heap\":%u,\"free_psram\":%u,"
                 "\"sd_state\":\"%s\",\"flash_used\":%d,\"jpeg_len\":%u,\"sensor\":\"%s\"}",
                 m.device_id, (long long)m.ts, m.time_valid ? "true" : "false",
                 (unsigned)m.boot_ms,
                 trig_src_str(src), m.rssi, (unsigned)m.free_heap, (unsigned)m.free_psram,
                 m.sd_state, m.flash_used, (unsigned)m.jpeg_len, m.sensor);
        esp_err_t e = sd_hal_write_file(meta_rel, meta, strlen(meta));
        if (e != ESP_OK) {
            LOGW(TAG, "meta.json write fail err=0x%x", e);
            /* 只是元数据失败，照片已经落盘，继续 */
        } else {
            LOGI(TAG, "meta ok path=/%s len=%u", meta_rel, (unsigned)strlen(meta));
        }
    }

    uint32_t out_id = 0;
    esp_err_t qe = upload_queue_push(sd_ok, jpg_rel, copy, fb->len, &m, &out_id);
    if (qe == ESP_OK) {
        queued = true;
        copy = NULL;           /* ★ 所有权已移交队列，这里必须置空，防止 done 里 double free */
        s_ok++;
    } else {
        LOGE(TAG, "enqueue fail err=0x%x (照片已丢弃)", qe);
        s_fail++;
    }

done:
    /* ---------- 6. 设备侧耗时（不含网络/上传，这才是"拍一张多久"的答案） ---------- */
    {
        int64_t t_end = esp_timer_get_time() / 1000;
        LOGI(TAG, "capture took=%lldms flash=%dms grab=%lldms retake=%d",
             (long long)(t_end - t_all0), flash_ms,
             (long long)(t_grab1 - t_grab0), retake);
    }

    /* ---------- 7. 唯一归还点 ---------- */
    /* 三条路径（grab 失败 / 已落盘 / 已拷贝）都汇聚到这里。
     * camera_hal_return(NULL) 是幂等的 no-op，所以不需要再判空分支。 */
    camera_hal_return(fb);

    /* copy != NULL 意味着"拷贝成功但入队失败"（队列满 drop_new 时队列自己已释放，
     * 所以只有 ESP_ERR_NO_MEM 之前的分支会走到这）。这里也是唯一释放点。 */
    if (copy) {
        free(copy);
    }

    /* 记录 PSRAM 最低水位，供 parse_log 统计 */
    {
        uint32_t cur = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (cur < s_psram_min) {
            s_psram_min = cur;
        }
        LOGI(TAG, "done psram=%u min=%u heap=%u",
             (unsigned)cur, (unsigned)s_psram_min, (unsigned)esp_get_free_heap_size());
    }

    /* ---------- 8. 深睡形态：判定无人 -> 本次唤醒没有后续工作了，回睡 -------
     *
     * ★ 必须放在**所有清理之后**（framebuffer 已归还、临时拷贝已释放）：
     *   sleep_mgr_sleep_now() 是**不返回**的，睡在清理之前会让"唯一归还点"
     *   永远跑不到，framebuffer 池直接被抽干。
     * ★ 只在"策略性丢弃"这条路上调用。入队成功的路径由**上传队列**在
     *   上传真正结束后调用（那时才真的没事干了，照片可能还在重试）。 */
    if (discarded_nonhuman) {
        sleep_mgr_notify_idle();
    }
    return queued;
}

const uint8_t *snapshot_pipeline_last(char *out_path, size_t path_sz, size_t *out_len)
{
    if (out_path && path_sz) {
        strlcpy(out_path, s_last_path, path_sz);
    }
    if (out_len) {
        *out_len = s_last_raw_len;
    }
    /* 只有在"当前已降级"时才返回内存副本；SD 正常时让调用方去读文件 */
    sd_state_t sd = sd_hal_state();
    if (sd == SD_OK || sd == SD_LOWSPACE) {
        return NULL;
    }
    return s_last_raw;
}
