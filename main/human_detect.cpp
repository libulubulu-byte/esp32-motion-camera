/**
 * @file human_detect.cpp
 * @brief ESP-WHO / esp-dl 人脸检测链路的 C++ 封装（真模型推理，对应参考工程
 *        D:\Camer_esp-who\examples\human_face_recognition_custom_s3）
 *
 * ★★ 2026-09-28：本文件从 human_detect.c **改名而来**，扩展名必须是 .cpp，不是 .c！
 *
 *   原因：本文件调用的 esp-dl / human_face_detect 是 **C++ API** ——
 *     · `#include "human_face_detect.hpp"`（C++ 头）
 *     · `HumanFaceDetect *d = new HumanFaceDetect(...)`（new/delete/类）
 *     · `std::list<dl::detect::result_t> &res`（STL 容器）
 *   用 .c 编译时 GCC 按 C 语言处理，遇到 `#include <string>` 直接报
 *       dl_define.hpp:5:10: fatal error: string: No such file or directory
 *   因为 C 模式不会去找 C++ 标准库头文件。
 *
 *   这正是 `sdkconfig` 里长期是 `# CONFIG_SNAP_ENABLE_HUMAN_DETECT is not set` 的
 *   **真正原因** —— 不是"默认不想开"，而是**打开了根本编译不过**，所以一直关着。
 *   改名 .cpp 后 CMake 会自动用 C++ 编译器处理，本条链路才第一次真正可编译。
 *
 *   改名注意：对外符号（human_detect_init / _run / _is_ready / _release /
 *   _run_count / _hit_count / _err_count）在 human_detect.h 里都有 extern "C"，
 *   所以 human_detect.c → .cpp 对调用方（snapshot_pipeline.c）**完全透明**，
 *   C 侧链接不会因为 C++ name mangling 而找不到符号。
 *
 * ===========================================================================
 * 一、为什么中间要过一道 JPEG 解码
 * ===========================================================================
 *  参考工程里检测前不需要解码，因为它的取帧链路是
 *      LCD_CAM/DVP -> RGB565 -> who_frame_cap
 *  帧本来就是 RGB565 裸像素，esp-dl 的 ImagePreprocessor 直接吃。
 *
 *  但**本工程**的取帧链路是 esp32-camera 组件，输出的是 **JPEG**：
 *      esp_camera_fb_get() -> fb->format == PIXFORMAT_JPEG
 *  而 ImagePreprocessor 只接受 RGB565/GRAY/RGB888 等裸像素，
 *  喂 JPEG 进去会被当成一堆随机像素 → 检测结果全是噪声。
 *
 *  所以本模块在检测前**必须**解一次 JPEG。解完正好是连续的 RGB565，
 *  尺寸与 row_step 天然一致，直接构造 img_t 交给模型即可。
 *
 *  代价：VGA(640x480) 解码约 30~40ms + 600KB PSRAM 缓冲。
 *  如果以后想省掉这一步，把 camera 配置改成
 *      config.pixel_format = PIXFORMAT_RGB565
 *  然后直接从 fb->buf 建 img_t（内容与解出来的完全一致），
 *  但那样 JPEG 上传/预览链路也要跟着改，收益不大，暂不做。
 *
 *  ★★ 字节序：必须与模型声明一致，否则**恒判无人脸**（2026-09-28 踩过）★★
 *  解码时必须用 DL_IMAGE_CAP_RGB565_BIG_ENDIAN，因为模型自带的
 *  ImagePreprocessor 声明要的就是「大端 RGB565 + RGB 交换」
 *  （human_face_detect.cpp:58-59）。用 caps=0(小端) 会让每个像素高低字节颠倒、
 *  565 位域全错 -> 模型看到的是彩色噪声 -> 永远 faces=0。
 *  详见 run() 里 t0 上方那段长注释，改这里之前先读它。
 *
 * ===========================================================================
 * 二、模型与分区
 * ===========================================================================
 *  MSRMNP_S8_V1 = MSR(粗筛) + MNP(精筛) 两级 INT8 模型，经 espressif/human_face_detect
 *  组件加载。实测本工程走的是 **CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA**
 *  （见 build/config/sdkconfig.h:1017）—— 即模型**编进 app 二进制**，
 *  所以 human_face_det 那个 2MB 分区其实是**空的、没被使用**（浪费但无害）。
 *  ⚠️ 别被 README/旧注释里"烧到 human_face_det 分区"的说法误导：
 *     只有切成 CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_PARTITION 才需要单独烧模型。
 *
 * ===========================================================================
 * 三、线程模型
 * ===========================================================================
 *  只在 trigger 任务里串行调用（trigger_fsm.c 保证了这一点），
 *  内部一把互斥锁仅用于串行化"模型加载/释放"，不保护推理本身 ——
 *  若将来出现第二个调用者，需要把整个 run() 放进锁里。
 */

#include "human_detect.h"
#include "logger.h"
#include "config_store.h"   /* ★ 门控阈值 human_score_pct 从运行期配置读 */

#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#if CONFIG_SNAP_ENABLE_HUMAN_DETECT

/* C++ 头必须在 extern "C" 之外（它们自带 extern "C" 保护，不能包起来） */
#include "human_face_detect.hpp"
#include "dl_image_jpeg.hpp"
#include "dl_image_define.hpp"

static const char *TAG = LOG_T_PIPE;

/**
 * MSR（粗筛/提案级）固定阈值 —— ★ 不跟 MNP 共用 config 的门控阈值 ★
 *
 * 级联检测器里 MSR 与 MNP 的**分数尺度不同**，绑同一个值会把粗筛饿死：
 * 实测同一帧 "两级都 0.05 -> 5 框 top=1.000" 而 "两级都 0.30 -> 0 框"，
 * 且 score 与阈值无关 ⇒ 0 框只能是 MSR 阶段 0 候选（MNP 根本没机会跑）。
 * 现场表现就是"人明明在镜头前却 faces=0"。
 *
 * 所以 MSR 只负责"别漏"，压到很低；判定门槛交给 MNP（config.human_score_pct）。
 * MSR 候选上限由组件的 MSRPostprocessor(max_boxes=10) 兜住，不会爆。
 */
#define HD_MSR_SCORE_THR   0.05f

/* ------------------------------ 内部状态 ------------------------------ */

static HumanFaceDetect *s_detector;     /* lazy_load=true：run() 时才真正加载模型 */
static SemaphoreHandle_t s_lock;        /* 只保护加载/释放，见文件头"线程模型"    */

static uint32_t s_run_count;
static uint32_t s_hit_count;
static uint32_t s_err_count;

/** 显式返回值，避免调用方把 fail-open 当成"真检测到人" */
#define HD_FAIL_OPEN()  do { s_err_count++; return true; } while (0)

static void warn_fail_open(const char *why)
{
    /* ★ 用 warn 而非 error：链路仍然可用，只是这一张没做门控 */
    LOGW(TAG, "human detect unavailable (%s) -> fail-open，本张照片照常上传", why);
}

/* ------------------------------ 模型管理 ------------------------------ */

/** 调用前必须已持锁 */
static bool ensure_detector(void)
{
    if (s_detector) {
        return true;
    }
    /* lazy_load=true：构造本身很轻，真正加载推到首次 run()，
     * 这样如果启动阶段就能加载失败，也不会阻塞开机流程。 */
    s_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    if (!s_detector) {
        LOGE(TAG, "new HumanFaceDetect 失败：内部 RAM/PSRAM 不足");
        return false;
    }
    LOGI(TAG, "detector created (MSRMNP_S8_V1, lazy load)");
    return true;
}

esp_err_t human_detect_init(int timeout_ms)
{
    (void)timeout_ms;      /* 当前实现为同步加载，用不到超时参数 */

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = ensure_detector();

    /* 提前把模型加载出来（约 300~700ms），避免第一张照片触发 PIR 时才发现
     * 模型坏了/没烧进去。get_raw_model() 内部就是"没加载就先 load"。 */
    if (ok) {
        int64_t t0 = esp_timer_get_time();
        dl::Model *m = s_detector->get_raw_model(0);   /* idx=0 -> MSR 粗筛模型 */
        int64_t dt = (esp_timer_get_time() - t0) / 1000;
        if (!m) {
            LOGE(TAG, "模型加载失败：human_face_det 分区可能没烧写（idf.py flash）");
            delete s_detector;
            s_detector = NULL;
            ok = false;
        } else {
            LOGI(TAG, "模型就绪 %lldms，PSRAM 余量 %uKB",
                 (long long)dt,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
    }
    xSemaphoreGive(s_lock);

    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool human_detect_is_ready(void)
{
    return s_detector != NULL;
}

void human_detect_release(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_detector) {
        delete s_detector;      /* DetectWrapper 析构会 delete 内层 MSRMNP */
        s_detector = NULL;
    }
    LOGI(TAG, "模型已释放，PSRAM 余量 %uKB",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    xSemaphoreGive(s_lock);
}

/* ------------------------------ 主检测函数 ------------------------------ */

/**
 * @brief 检测主体：JPEG -> RGB565 -> MSR/MNP -> 结果
 *
 * ★ 2026-09-28 抽出来给两个入口共用（human_detect_run / human_detect_run_raw）。
 *   为什么要共用而不是复制一份：这个实验的全部价值就在于"两条路走的是**同一段**
 *   解码与推理代码"。一旦复制成两份，两者出现差异时就再也说不清问题出在
 *   输入还是算法 —— 那这个直灌实验就白做了。
 *
 * @param from_camera 仅用于日志标注（camera / inject），不改变任何行为
 */
static bool do_detect(const void *jpeg, size_t len, human_detect_result_t *out, const char *from_camera)
{
    human_detect_result_t r = {};
    r.human = true;                     /* 默认 fail-open */

    if (!jpeg || len == 0) {
        warn_fail_open("empty frame");
        HD_FAIL_OPEN();
    }

    /* 懒初始化：调用方忘了 init 也不会静默降级 */
    if (!s_lock && human_detect_init(0) != ESP_OK) {
        warn_fail_open("init failed");
        HD_FAIL_OPEN();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = ensure_detector();
    xSemaphoreGive(s_lock);
    if (!ok) {
        warn_fail_open("detector unavailable");
        HD_FAIL_OPEN();
    }

    /* ---------- 1. JPEG -> RGB565（esp-dl 自带封装，内部管理解码器生命周期） ---------- */
    dl::image::jpeg_img_t jimg = { .data = (void *)jpeg, .data_len = len };

    /* ★★ 2026-09-28 【真机 bug：永远 faces=0 的根因】★★
     *
     * 现场症状：人站在镜头正前方、光照正常，短按 6 次，**每一次**都是
     *    人脸检测 MISS faces=0 best=0
     * 模型没坏、帧是新鲜的（age=70ms 量级）、解码也成功 —— 但一个脸都检不出。
     *
     * 根因：**解码出来的 RGB565 字节序与模型声明的不一致**。
     *
     *   human_face_detect.cpp:58-59（ESP32-S3 分支）里，模型自带的预处理器声明的是：
     *       new dl::image::ImagePreprocessor(m_model, {0,0,0}, {1,1,1},
     *           DL_IMAGE_CAP_RGB_SWAP | DL_IMAGE_CAP_RGB565_BIG_ENDIAN);
     *                                                       ^^^^^^^^^^^^^^^^^^^^^^^^
     *   它**要求输入是「大端 RGB565 + RGB 交换」**。
     *
     *   而这里原来用 caps=0 解码 = **小端(LE) RGB565**（见 dl_image_jpeg.cpp:25-28，
     *   caps 默认 0 -> JPEG_PIXEL_FORMAT_RGB565_LE）。
     *
     *   ⇒ 喂给模型的每个像素**高/低字节是反的**：R/B 通道错位、565 的位域全部解析错，
     *     模型输入等于彩色噪声，于是恒判"无人脸"。这和亮度/曝光/距离都无关，
     *     所以怎么站、怎么补光都永远是 0 —— 正是本次现场现象的特征。
     *
     * 判据（为什么确定是字节序而不是别的）：
     *   · 解码失败会打 "Failed to decode jpeg." + 走 fail-open（照片会照常上传），
     *     而现场是 MISS + 丢弃 ⇒ 解码成功；
     *   · 帧不新鲜会打 "drop stale frame" 且 age 很大，而现场 age=72/36/37ms ⇒ 帧是新的；
     *   · 剩下的唯一环节就是**像素格式**，而它恰好与模型声明相反。
     *
     * 修法：解码时**显式声明 BIG_ENDIAN**，与模型预处理器的要求对齐。
     *   ⚠️ 不要改回 caps=0（LE）。若将来把模型换成别的 esp-dl 模型，
     *      要去 human_face_detect.cpp 里看它 ImagePreprocessor 用的到底是哪个 cap，
     *      以那边的声明为准，不要照抄这里的值。
     *
     * 注：sw_decode_jpeg 的 assert 允许 caps ∈ {0, DL_IMAGE_CAP_RGB565_BIG_ENDIAN}，
     *     所以这里传 BIG_ENDIAN 是合法的。（RGB_SWAP 那一半由模型侧的
     *     ImagePreprocessor 自己在预处理时做，解码器不管 R/B 顺序。） */
    int64_t t0 = esp_timer_get_time();
    dl::image::img_t img = dl::image::sw_decode_jpeg(
        jimg, dl::image::DL_IMAGE_PIX_TYPE_RGB565, dl::image::DL_IMAGE_CAP_RGB565_BIG_ENDIAN);
    int64_t t1 = esp_timer_get_time();

    if (!img.data || img.width == 0 || img.height == 0) {
        warn_fail_open("jpeg decode failed");
        HD_FAIL_OPEN();
    }

    /* ---------- 2. 推理（两阶段：MSR 粗筛 -> MNP 精筛） ---------- */
    const uint16_t img_w = img.width;
    const uint16_t img_h = img.height;

    /* ★★ 阈值探针：MSR 对"这一张图"到底有没有反应？★★
     *
     * 这个探针是本次排查留下的**最有价值的一条**，别删。
     *
     * 已实测到的事实（2026-09-28）：
     *   · 本模型 msr_input shape=[1,120,160,3]，与组件 README 表格
     *     msr_s8_v1_s3 的 "120*160*3" **完全一致** ⇒ 输入尺寸没问题；
     *   · 同一张实拍图：score_thr=0.05 时 MSR 吐 4 个框、score 全 0.999~1.000；
     *     而 score_thr=0.5 时 **0 个框**。⇒ 阈值与量化尺度对不上。
     *
     * 之所以要打印多档而只不是一次 run 的结果：只跑一档只知道"过或不过"，
     * 多档才能把**拐点**夹出来，并直接暴露"满分框被 0.5 挡掉"这种荒唐情况。
     *
     * ⚠️ 它每次检测都会多跑几次推理（每次 ~60ms）。直灌调试期完全值得；
     *    若以后要在生产固件里关掉，把 k_diag_probe 置 false 即可。 */
    /* ★★ 2026-09-28：MSR(粗筛) 与 MNP(精筛) 的阈值**必须分开设** ★★
     *
     * 这是本次排查最重要的一个认知。原先两级都写 0.5（后来都写 config 的同一个值），
     * 等于用**判定级的门槛去卡提案级**，直接把粗筛饿死：
     *
     *   实测同一帧：  两级都 0.05  -> 框数=5  top=1.000
     *                 两级都 0.30  -> 框数=0（**MSR 阶段就 0 候选**）
     *
     *   score = sigmoid(dequantize(raw)) 与阈值无关，那 5 个 1.000 的框在 0.30 下
     *   本应存活 ⇒ 0 候选只能说明 MSR 没吐出候选，MNP 根本没机会跑。
     *   现场表现就是"人明明在镜头前却 faces=0"。
     *
     * 级联检测器的正确用法：**MSR 放宽保证"不漏"，MNP 收紧保证"不准"**。
     *   于是 MSR 固定用 HD_MSR_SCORE_THR（很低），MNP 用 config 的
     *   human_score_pct —— 后者才是真正的判定门槛。
     *
     * 调法（**不必重编译**）：
     *     POST /api/config   {"human_score_pct":25}     # 越小越灵敏
     */
    const float score_thr = (float)config_store_get()->human_score_pct / 100.0f;
    s_detector->set_score_thr(HD_MSR_SCORE_THR, 0);       /* idx=0 -> MSR 粗筛：固定放宽 */
    s_detector->set_score_thr(score_thr, 1);              /* idx=1 -> MNP 精筛：config 可调 */

    /* 灵敏度探针：用**最乐观**的阈值（两级都 0.05）再跑一次，把能达到的最高分打出来。
     * 即使本次 MISS，也能一眼区分两种完全不同的原因：
     *     top 高  ⇒ 模型看得到脸，是被 MNP 门槛挡了 -> 调低 human_score_pct
     *     top≈0   ⇒ 画面里根本没脸（太远/太暗/太偏） -> 改拍摄条件
     * 只跑 1 档（约 +60ms）。 */
    static const bool k_diag_probe = true;
    if (k_diag_probe) {
        s_detector->set_score_thr(0.05f, 0);
        s_detector->set_score_thr(0.05f, 1);
        std::list<dl::detect::result_t> &o = s_detector->run(img);
        /* 同时打最高分：光有框数看不出"差一点"还是"差很多" */
        float top = 0.f;
        for (auto &it : o) {
            if (it.score > top) top = it.score;
        }
        LOGI(TAG, "PROBE[%s] 最乐观(0.05/0.05) -> 框数=%d top=%.3f (MNP门控=%.2f)",
             from_camera, (int)o.size(), (double)top, (double)score_thr);
        s_detector->set_score_thr(HD_MSR_SCORE_THR, 0);   /* 恢复：MSR 放宽 */
        s_detector->set_score_thr(score_thr, 1);          /*         MNP 门控 */
    }

    int64_t t2 = esp_timer_get_time();
    std::list<dl::detect::result_t> &res = s_detector->run(img);
    int64_t t3 = esp_timer_get_time();

    int best = 0;
    for (const auto &b : res) {
        int s = (int)(b.score * 100.f + 0.5f);
        if (s > best) {
            best = s;
        }
    }
    if (best > 100) {
        best = 100;
    }

    r.human     = !res.empty();
    r.score     = best;
    r.num_faces = (int)res.size();
    /* ★ 2026-09-28 修正：t0..t3 是 **微秒**（esp_timer_get_time() 的单位），
     *   而这两个字段是 **毫秒**，原来漏了 /1000，导致日志打成
     *       decode=63053ms infer=62956ms
     *   而整条流水线 capture took 只有 546ms —— 数字比总耗时还大 100 倍，
     *   就是这个 1000 倍单位错误造成的。单位换算只在这一处做，别再漏。 */
    r.decode_ms = (int)((t1 - t0) / 1000);
    r.infer_ms  = (int)((t3 - t2) / 1000);

    if (out) {
        *out = r;
    }
    s_run_count++;
    if (r.human) {
        s_hit_count++;
    }

    /* ★★ 2026-09-28 诊断：光有 "faces=0" 无法区分"模型坏了"和"画面不对" ★★
     *
     * 现场困了两轮：帧是新的（age=36ms）、分辨率对（640x480）、解码成功
     * （decode=61ms）、推理真跑了（infer=61ms）、模型也确实完整嵌入
     * （map 里 191248B = msr 61168 + mnp 129968 + 打包头），可就是 faces=0。
     *
     * 剩下的可能只有两类，而它们的**修法完全不同**：
     *   (a) **像素不对**（字节序 / R-B 交换 / 我喂进去的就是彩色噪声）
     *       ⇒ 模型再怎么调都没用，要改解码 caps；
     *   (b) **画面不对**（脸太小 / 太糊 / 补光把脸打爆 / 角度太偏）
     *       ⇒ 模型没问题，要改拍摄条件。
     *
     * 所以这里必须把**模型实际吃到的那张图**用可判读的方式打出来：
     *   · MSR 模型输入张量的真实 shape（我看不到它就无法判断缩放比是否离谱）；
     *   · 解码后“脸部中心区域”的若干采样像素（RGB565 -> R/G/B），
     *     人脸正常时应满足 R>G>B 且三通道有层次；若是噪声/位序错，
     *     会呈现三通道混乱或恒定值（例如全 0 或全 0xF8 一片白）。
     *
     * ⚠️ 这段是**排查期**的日志，代价只有几十微秒（几次内存读 + 一行 printf），
     *   但会在每次检测时刷屏。门控调通后可以整段删掉（搜索 "诊断"）。
     *
     * 采样点为什么取画面正中偏上：短按按钮时人脸基本在取景框中央，
     *   取 (w/2, h*0.42) 这个"人眼高度"位置比几何中心更能反映肤色。 */
    {
        /* 1) 模型输入的真实 shape —— MSR 粗筛是正方形输入，这里能看到具体边长。
         *
         * 拿它的正确姿势（我第一版写错过两次，记下来别再绕）：
         *   DetectWrapper::get_raw_model(idx) 的返回值已经是 **dl::Model ***
         *   （见 dl_detect_base.hpp 的虚函数签名），不是内层 MSRMNP 对象，
         *   所以直接调 ->get_input() 即可，不要再套一层 get_raw_model。 */
        dl::Model *msr_model = s_detector ? s_detector->get_raw_model(0) : nullptr;
        dl::TensorBase *msr_in = msr_model ? msr_model->get_input() : nullptr;
        if (msr_in) {
            LOGI(TAG, "DIAG msr_input shape=[%d,%d,%d,%d] dtype=%d exp=%d",
                 (int)msr_in->shape[0], (int)msr_in->shape[1],
                 (int)msr_in->shape[2], (int)msr_in->shape[3],
                 (int)msr_in->dtype, (int)msr_in->exponent);

            /* ★★ MSR 的两级输出张量才是"候选框为 0"的直接证据 ★★
             *
             * 为什么必须看它：MSRPostprocessor::parse_stage() 里（dl_detect_msr_postprocessor.cpp:19-22）
             *     int H = score->shape[1];   // 特征图高
             *     int W = score->shape[2];   // 特征图宽
             *     int A = anchor_shape.size();
             *     int C = score->shape[3] / A;
             * 也就是说锚框数量 A 与 **输出张量形状** 必须匹配；
             * 而 human_face_detect.cpp:40 给 MSR 配的是 stride 8/16 + anchor
             * {16,16},{32,32},{64,64},{128,128} —— 这套是给**大输入**调的。
             *
             * 本工程的输入只有 160x120（见上面 msr_input），特征图只有 20x15 / 10x8。
             * 若模型输出张量的通道数/形状与这套 anchor 对不上，
             * parse_stage 里的 `C = shape[3]/A` 会算出畸形值 —— 遍历就取不到任何
             * 有效通道，score 永远 <= 阈值 ⇒ candidates 恒为空 ⇒ MNP 空转 ⇒ faces=0。
             *
             * 而 "detect pre/model/post" 三行日志的打印条件是
             * candidates.size() > 0（human_face_detect.cpp:117），现场一条都没有，
             * 正好印证 candidates 恒空。把 score0/box0 的形状打出来就是最后一块拼图。 */
            const char *names[2] = {"score0", "score1"};
            for (int i = 0; i < 2; i++) {
                dl::TensorBase *sc = msr_model->get_output(names[i]);
                if (sc) {
                    LOGI(TAG, "DIAG MSR %s shape=[%d,%d,%d,%d]", names[i],
                         (int)sc->shape[0], (int)sc->shape[1],
                         (int)sc->shape[2], (int)sc->shape[3]);
                } else {
                    LOGW(TAG, "DIAG MSR 输出 '%s' 取不到", names[i]);
                }
            }
        } else {
            LOGW(TAG, "DIAG msr_input 不可用（拿不到张量，模型可能没加载成功）");
        }

        /* 2) 解码后图像在"脸应当出现的位置"的采样像素。
         *    注意 img.data 是按 RGB565 大端存的：高字节在前。 */
        const uint8_t *px = (const uint8_t *)img.data;
        const int sx = (int)img_w / 2;
        const int sy = (int)((float)img_h * 0.42f);
        if (px && sx < img_w && sy < img_h) {
            const uint8_t *p = px + ((size_t)sy * img_w + sx) * 2;
            uint16_t v = (uint16_t)((p[0] << 8) | p[1]);   /* 大端 */
            int R = ((v >> 11) & 0x1F) << 3;
            int G = ((v >> 5)  & 0x3F) << 2;
            int B = (v & 0x1F) << 3;
            LOGI(TAG, "DIAG px@(%d,%d) raw=0x%04X R=%d G=%d B=%d %s",
                 sx, sy, (unsigned)v, R, G, B,
                 (R > G && G > B) ? "(暖色/疑似肤色)" : "(非肤色顺序)");
        }
    }

    LOGI(TAG, "人脸检测[%s] %s faces=%d best=%d (decode=%dms infer=%dms %ux%u)",
         from_camera, r.human ? "HIT " : "MISS", r.num_faces, r.score,
         r.decode_ms, r.infer_ms, (unsigned)img_w, (unsigned)img_h);

    /* ★ 解码缓冲是 sw_decode_jpeg 内部 heap_caps_aligned_alloc 出来的，
     *   调用方必须自己释放；放在日志之后是为了日志里还能用到宽高。 */
    heap_caps_free(img.data);

    return r.human;
}

/* ------------------------------ 对外入口 ------------------------------ */

/* 相机路径：设备自己拍的帧（产品流程走这条） */
bool human_detect_run(const void *jpeg, size_t len, human_detect_result_t *out)
{
    return do_detect(jpeg, len, out, "camera");
}

/* ★ 直灌路径：把 PC 端传入的 JPEG 当一帧检测（调试专用，见 human_detect.h 说明）。
 *
 * 为什么统计计数**不加**在这里：run/hit/err 是 /api/status 上给用户看的
 * "设备自己抓拍"的统计，把调试灌进来的图算进去会污染它（灌 10 张 Lena
 * 就把 hit 刷成 10，看起来像现场检出了 10 次人）。所以直灌只打日志、不计数。
 * 这也意味着它**不能**用来验证 /api/status 的计数逻辑 —— 那要用真拍。 */
bool human_detect_run_raw(const void *jpeg, size_t len, human_detect_result_t *out)
{
    uint32_t run0 = s_run_count, hit0 = s_hit_count;
    bool ret = do_detect(jpeg, len, out, "inject");
    s_run_count = run0;      /* 回滚：直灌不计入产品统计 */
    s_hit_count = hit0;
    return ret;
}

uint32_t human_detect_run_count(void) { return s_run_count; }
uint32_t human_detect_hit_count(void) { return s_hit_count; }
uint32_t human_detect_err_count(void) { return s_err_count; }

#else  /* !CONFIG_SNAP_ENABLE_HUMAN_DETECT ---------------------------------- */

/* 关掉时也导出同名符号，调用方不必到处写 #if */

static const char *TAG = LOG_T_PIPE;

esp_err_t human_detect_init(int timeout_ms)
{
    (void)timeout_ms;
    LOGW(TAG, "human_detect 未编译（CONFIG_SNAP_ENABLE_HUMAN_DETECT=n）");
    return ESP_ERR_NOT_SUPPORTED;
}

bool human_detect_is_ready(void) { return false; }

bool human_detect_run(const void *jpeg, size_t len, human_detect_result_t *out)
{
    (void)jpeg;
    (void)len;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->human = true;      /* 与打开时语义一致：不做门控，照片照传 */
    }
    return true;
}

/* 直灌入口在关闭态下也要导出同名符号，否则 web 端 /api/detect_raw 链接不过 */
bool human_detect_run_raw(const void *jpeg, size_t len, human_detect_result_t *out)
{
    (void)jpeg;
    (void)len;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->human = true;
    }
    return true;
}

uint32_t human_detect_run_count(void) { return 0; }
uint32_t human_detect_hit_count(void) { return 0; }
uint32_t human_detect_err_count(void) { return 0; }

#endif /* CONFIG_SNAP_ENABLE_HUMAN_DETECT */
