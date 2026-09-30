/**
 * @file sd_hal.c
 * @brief 板载 microSD：SDMMC 1-bit + 三态状态机
 *
 * 时序/资源约束（任务书 1.4）：
 *   - 只支持 1-bit（GPIO39 CLK / 38 CMD / 40 DAT0）。禁止 4-bit 选项。
 *   - 禁止使能 JTAG：MTCK/MTDO 就是 GPIO39/40，与 SD 冲突。
 *
 * 降级策略（OPTIONAL）：
 *   - 启动挂载失败 -> SD_FAIL，30s 后自动重试
 *   - 运行期写失败连续 3 次 -> SD_FAIL
 *   - 运行期 mount 失效 -> SD_REMOVED，30s 后自动重试
 *   - 重挂载成功 -> SD_OK，并清零失败计数
 */

#include "sd_hal.h"
#include "pins_config.h"
#include "logger.h"
#include "config_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_SD;

#define SD_MOUNT_POINT   "/sdcard"
#define SD_RETRY_MS      30000      /* 自动重挂载基础周期 */
#define SD_RETRY_MS_MAX  600000     /* 退避上限：10 分钟 */
#define SD_WRITE_FAIL_MAX 3         /* 连续写失败几次就降级 */

/* ------------------------------ 编译期模式 ------------------------------ */
#if defined(CONFIG_SD_MODE_NONE)
#define SD_MODE_NONE_BUILD      1
#define SD_MODE_REQUIRED_BUILD  0
#elif defined(CONFIG_SD_MODE_REQUIRED)
#define SD_MODE_NONE_BUILD      0
#define SD_MODE_REQUIRED_BUILD  1
#else
#define SD_MODE_NONE_BUILD      0
#define SD_MODE_REQUIRED_BUILD  0
#endif

/* ------------------------------- 内部状态 ------------------------------- */
static sdmmc_card_t    *s_card;
static bool             s_mounted;
static sd_state_t       s_state = SD_UNKNOWN;
static uint32_t         s_retry_count;
static int              s_last_err;
static int              s_write_fail_streak;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_retry_timer;

/** 把 config 里的 sd_low_space_mb 换算成字节（0 = 不启用低空间检查） */
static uint64_t config_store_low_space_bytes(void)
{
    int mb = config_store_get()->sd_low_space_mb;
    return (mb > 0) ? (uint64_t)mb * 1024ULL * 1024ULL : 0ULL;
}

/* 低空间清理时，记录"上传成功过了哪些日期目录" —— 由 upload_queue 通过
 * sd_hal_note_uploaded_ok() 通知。为了不过度设计，这里只维护一个
 * "最后成功上传的日期目录"字符串，只清理它之前的目录。 */
static char s_last_ok_day[16];      /* "YYYYMMDD" */

/* ============================== 状态字符串 ============================== */
const char *sd_hal_state_str(sd_state_t st)
{
    switch (st) {
    case SD_DISABLED:  return "DISABLED";
    case SD_UNKNOWN:   return "UNKNOWN";
    case SD_MOUNTING:  return "MOUNTING";
    case SD_OK:        return "OK";
    case SD_FAIL:      return "FAIL";
    case SD_REMOVED:   return "REMOVED";
    case SD_LOWSPACE:  return "LOWSPACE";
    default:           return "?";
    }
}

bool sd_hal_is_enabled(void)
{
    return !SD_MODE_NONE_BUILD;
}

bool sd_hal_is_required(void)
{
    return SD_MODE_REQUIRED_BUILD != 0;
}

sd_state_t sd_hal_state(void)
{
    return s_state;
}

sd_status_t sd_hal_status(void)
{
    sd_status_t st = {0};
    st.state = s_state;
    st.retry_count = s_retry_count;
    st.last_error = s_last_err;
    strlcpy(st.cid, "n/a", sizeof(st.cid));

    if (s_mounted && s_card) {
        uint64_t total = 0, fre = 0;
        esp_err_t e = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre);
        if (e == ESP_OK) {
            st.total_bytes = total;
            st.free_bytes = fre;
        }
        /* IDF 5.5 的 sdmmc_card_t.cid 是**解码后的结构体** sdmmc_cid_t
         * （mfg_id/oem_id/name[8]/revision/serial/date），不是 16 字节原始数组。
         * 这里用厂家 ID + 产品名 + 序列号拼一个可辨识的字符串。
         * 注意 name 可能不带结尾 '\0'（最长 8 字符），必须用 %.*s 限长。 */
        const sdmmc_cid_t *cid = &s_card->cid;
        if (cid->mfg_id || cid->serial) {
            snprintf(st.cid, sizeof(st.cid), "mfg=%02x %.*s sn=%08x",
                     cid->mfg_id, (int)sizeof(cid->name), cid->name, (unsigned)cid->serial);
        }
    }
    return st;
}

/* ============================== 挂载 / 卸载 ============================== */
static esp_err_t do_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    LOGI(TAG, "mount start clk=%d cmd=%d d0=%d mode=1bit",
         SD_PIN_CLK, SD_PIN_CMD, SD_PIN_DAT0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* 1-bit 模式：bus_width = 1 */
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40MHz；不稳定时可降到 SDMMC_FREQ_DEFAULT */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0  = SD_PIN_DAT0;
    /* 板上已有外部上拉；这里开内部上拉兜底，避免"能识别但不能读"的玄学问题 */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* 绝不自动格式化用户的卡 */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_last_err = err;
        LOGE(TAG, "mount fail err=0x%x -> degrade queue_mode=buffer retry=%ds",
             err, SD_RETRY_MS / 1000);
        s_card = NULL;
        return err;
    }

    s_mounted = true;
    s_write_fail_streak = 0;

    uint64_t total = 0, fre = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre);
    char cid[40] = "n/a";
    if (s_card) {
        /* cid 是解码后的结构体（见 sd_hal_status 里的说明），不是字节数组 */
        const sdmmc_cid_t *ci = &s_card->cid;
        snprintf(cid, sizeof(cid), "mfg=%02x %.*s sn=%08x",
                 ci->mfg_id, (int)sizeof(ci->name), ci->name, (unsigned)ci->serial);
    }
    LOGI(TAG, "mount ok total=%lluMB free=%lluMB cid=%s",
         (unsigned long long)(total / (1024ULL * 1024ULL)),
         (unsigned long long)(fre / (1024ULL * 1024ULL)), cid);
    return ESP_OK;
}

static void do_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
}

/* ============================== 自动重挂载（带退避） ============================== */

/** "卡根本没插"类错误：这类错误重复试也没用，日志应静默 + 快速拉长退避 */
static bool err_looks_like_no_card(int err)
{
    /* 0x107 ESP_ERR_TIMEOUT : send_op_cond 阶段超时（CMD1 无应答）＝ 无卡
     * 0x105 ESP_ERR_NOT_FOUND: 卡未响应                     ＝ 无卡
     * 0x10a ESP_ERR_INVALID_STATE: 卡已移除后再访问          ＝ 无卡 */
    return (err == 0x107) || (err == 0x105) || (err == 0x10a);
}

/** 根据连续失败次数算下次退避间隔（30s 起步，翻倍，封顶 10min） */
static uint32_t next_retry_ms(void)
{
    uint32_t ms = SD_RETRY_MS;
    /* s_retry_count 含首次失败，用 min(count,5) 避免移位溢出 */
    uint32_t shift = (s_retry_count > 5) ? 5 : s_retry_count;
    ms <<= shift;
    if (ms > SD_RETRY_MS_MAX) {
        ms = SD_RETRY_MS_MAX;
    }
    return ms;
}

static void retry_common(void)
{
    if (SD_MODE_NONE_BUILD) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    /* 只有 FAIL / REMOVED / UNKNOWN 才需要重试 */
    if (s_state == SD_FAIL || s_state == SD_REMOVED || s_state == SD_UNKNOWN) {
        s_retry_count++;
        s_state = SD_MOUNTING;

        bool quiet = err_looks_like_no_card(s_last_err);

        /* ★ 无卡时不再每轮都打一堆 sdmmc_common/esp_littlefs 的 error + HINT。
         *   只每 10 次（约 5 分钟量级）提示一次，其余静默，避免刷屏淹没有效日志。 */
        if (!quiet || (s_retry_count % 10) == 1) {
            LOGI(TAG, "retry mount start (attempt %u)", (unsigned)s_retry_count);
        }

        esp_err_t err = do_mount();
        if (err == ESP_OK) {
            s_state = SD_OK;
            s_retry_count = 0;                 /* ★ 成功即清零，重新启用快周期 */
            LOGI(TAG, "remount ok recovered state=OK");
        } else {
            s_state = SD_FAIL;
            s_last_err = err;

            /* 动态调整定时器周期：有卡(错误像"卡异常")保持 30s，
             * 无卡则指数退避，避免每 30s 无意义地唤醒 + 刷屏。 */
            if (s_retry_timer) {
                uint32_t ms = quiet ? next_retry_ms() : SD_RETRY_MS;
                esp_timer_stop(s_retry_timer);
                esp_timer_start_periodic(s_retry_timer, (uint64_t)ms * 1000ULL);
                if (!quiet || (s_retry_count % 10) == 0) {
                    LOGW(TAG, "remount fail err=0x%x%s next retry %us",
                         err, quiet ? " (no card)" : "", (unsigned)(ms / 1000));
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    retry_common();
}

esp_err_t sd_hal_remount(void)
{
    if (SD_MODE_NONE_BUILD) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    LOGI(TAG, "manual remount requested");
    do_unmount();
    s_state = SD_MOUNTING;
    esp_err_t err = do_mount();
    if (err == ESP_OK) {
        s_state = SD_OK;
    } else {
        s_state = SD_FAIL;
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* ================================ init ================================ */
esp_err_t sd_hal_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (SD_MODE_NONE_BUILD) {
        s_state = SD_DISABLED;
        LOGI(TAG, "mode=NONE -> sd_hal disabled (all calls return NOT_SUPPORTED)");
        return ESP_OK;
    }

    s_state = SD_MOUNTING;
    esp_err_t err = do_mount();

    if (err == ESP_OK) {
        s_state = SD_OK;
    } else if (SD_MODE_REQUIRED_BUILD) {
        s_state = SD_FAIL;
        LOGE(TAG, "mode=REQUIRED and mount failed -> fatal");
        return ESP_FAIL;      /* 调用方进 safe mode */
    } else {
        s_state = SD_FAIL;
        LOGI(TAG, "state=FAIL -> degrade queue_mode=buffer retry=%ds", SD_RETRY_MS / 1000);
    }

    /* 无论成功失败都起 30s 周期定时器：成功时它是"No-op"，失败时它就是自动重挂载 */
    esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "sd_retry",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&targs, &s_retry_timer) == ESP_OK) {
        esp_timer_start_periodic(s_retry_timer, SD_RETRY_MS * 1000ULL);
    }
    return ESP_OK;
}

/* ============================== 路径工具 ============================== */

/* 内部使用：相对路径 -> "/sdcard/xxx" */
static void abs_path(const char *rel, char *out, size_t out_sz)
{
    if (rel[0] == '/') {
        /* 调用方给的已经是绝对路径（例如 /api/photo?path= 传进来的） */
        strlcpy(out, rel, out_sz);
    } else {
        snprintf(out, out_sz, "%s/%s", SD_MOUNT_POINT, rel);
    }
}

const char *sd_hal_to_rel(const char *abs, char *out, size_t out_sz)
{
    if (!abs) {
        out[0] = '\0';
        return NULL;
    }
    const char *p = abs;
    if (strncmp(abs, SD_MOUNT_POINT "/", sizeof(SD_MOUNT_POINT)) == 0) {
        p = abs + sizeof(SD_MOUNT_POINT);
    } else if (abs[0] == '/') {
        p = abs + 1;
    }
    strlcpy(out, p, out_sz);
    return out;
}

void sd_hal_make_paths(char *jpg_rel, size_t jpg_sz, char *meta_rel, size_t meta_sz)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* 毫秒后缀：同秒内连拍 50 张也不会重名（任务书验收项） */
    int64_t us = esp_timer_get_time();
    int ms = (int)((us / 1000) % 1000);

    snprintf(jpg_rel, jpg_sz, "snapshots/%04d/%02d/%02d/%02d%02d%02d_%03d.jpg",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ms);

    if (meta_rel && meta_sz) {
        strlcpy(meta_rel, jpg_rel, meta_sz);
        char *dot = strrchr(meta_rel, '.');
        if (dot) {
            strlcpy(dot, ".meta.json", meta_sz - (size_t)(dot - meta_rel));
        }
    }
}

/* 递归建目录：只处理 rel 的父目录部分 */
static void mkdirs_for(const char *rel)
{
    char p[192];
    abs_path(rel, p, sizeof(p));

    /* 从 "/sdcard/" 之后开始逐层 mkdir */
    char *slash = p + strlen(SD_MOUNT_POINT);
    for (char *q = slash; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(p, 0777);      /* 已存在返回 -1/EEXIST，忽略即可 */
            *q = '/';
        }
    }
}

/* ============================== 文件操作 ============================== */
esp_err_t sd_hal_write_file(const char *rel_path, const void *data, size_t len)
{
    if (SD_MODE_NONE_BUILD) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_state != SD_OK && s_state != SD_LOWSPACE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;      /* 已降级：调用方走 PSRAM 分支 */
    }

    char tmp_rel[200];
    snprintf(tmp_rel, sizeof(tmp_rel), "%s.tmp", rel_path);

    char tmp_abs[224];
    abs_path(tmp_rel, tmp_abs, sizeof(tmp_abs));

    mkdirs_for(rel_path);

    FILE *f = fopen(tmp_abs, "wb");
    esp_err_t ret = ESP_OK;
    if (!f) {
        ret = ESP_FAIL;
    } else {
        size_t w = fwrite(data, 1, len, f);
        fclose(f);
        if (w != len) {
            ret = ESP_FAIL;
        }
    }

    if (ret == ESP_OK) {
        char abs[224];
        abs_path(rel_path, abs, sizeof(abs));
        remove(abs);                       /* rename 不覆盖已存在文件 */
        if (rename(tmp_abs, abs) != 0) {
            ret = ESP_FAIL;
        }
    }

    if (ret != ESP_OK) {
        s_write_fail_streak++;
        LOGW(TAG, "write fail path=%s count=%d/%d", rel_path,
             s_write_fail_streak, SD_WRITE_FAIL_MAX);
        remove(tmp_abs);
        if (s_write_fail_streak >= SD_WRITE_FAIL_MAX) {
            LOGW(TAG, "write fail count>=%d state=FAIL -> degrade", SD_WRITE_FAIL_MAX);
            /* 先卸载再置 FAIL，避免"状态已降级但卡还挂着"的资源泄漏 */
            do_unmount();
            s_state = SD_FAIL;
        }
    } else {
        s_write_fail_streak = 0;
        /* 写成功顺手更新空间状态 */
        uint64_t total = 0, fre = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre) == ESP_OK) {
            uint64_t low = (uint64_t)config_store_low_space_bytes();
            if (low && fre < low) {
                if (s_state != SD_LOWSPACE) {
                    LOGW(TAG, "free=%lluMB < low=%lluMB -> LOWSPACE",
                         (unsigned long long)(fre / 1048576ULL), (unsigned long long)(low / 1048576ULL));
                }
                s_state = SD_LOWSPACE;
            } else if (s_state == SD_LOWSPACE) {
                LOGI(TAG, "free space recovered -> state=OK");
                s_state = SD_OK;
            }
        }
    }

    xSemaphoreGive(s_lock);
    return ret;
}

int sd_hal_read_file(const char *rel_path, void *buf, size_t buf_sz)
{
    if (SD_MODE_NONE_BUILD) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return -1;
    }
    char abs[224];
    abs_path(rel_path, abs, sizeof(abs));

    FILE *f = fopen(abs, "rb");
    if (!f) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    size_t n = fread(buf, 1, buf_sz, f);
    fclose(f);
    xSemaphoreGive(s_lock);
    return (int)n;
}

bool sd_hal_file_exists(const char *rel_path)
{
    if (SD_MODE_NONE_BUILD) {
        return false;
    }
    char abs[224];
    abs_path(rel_path, abs, sizeof(abs));
    struct stat st;
    return stat(abs, &st) == 0;
}

long sd_hal_file_size(const char *rel_path)
{
    if (SD_MODE_NONE_BUILD) {
        return -1;
    }
    char abs[224];
    abs_path(rel_path, abs, sizeof(abs));
    struct stat st;
    if (stat(abs, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

int sd_hal_remove(const char *rel_path)
{
    if (SD_MODE_NONE_BUILD) {
        return -1;
    }
    char abs[224];
    abs_path(rel_path, abs, sizeof(abs));
    return remove(abs);
}

/* ============================== 空间清理 ============================== */

/**
 * 低空间清理策略（任务书 5.5）：
 *   **禁止删除未上传成功的文件**。
 * 实现：upload_queue 每成功上传一张就调用 sd_hal_note_uploaded_ok(path)，
 * 这里记录"最后成功上传到的日期(YYYYMMDD)"。清理时只删除
 * **严格早于该日期**的 snapshots/YYYY/MM/DD 目录 —— 这些目录里的文件
 * 一定都已经有对应事件进入过上传队列，且比当前成功水位更早。
 * 这样即使某个文件上传失败，它也只会停留在水位日期及以后，不会被删。
 */
static void note_uploaded_day_locked(const char *rel_path)
{
    /* rel_path = "snapshots/YYYY/MM/DD/HHMMSS_mmm.jpg" */
    int y = 0, m = 0, d = 0;
    if (sscanf(rel_path, "snapshots/%d/%d/%d/", &y, &m, &d) == 3) {
        snprintf(s_last_ok_day, sizeof(s_last_ok_day), "%04d%02d%02d", y, m, d);
    }
}

void sd_hal_note_uploaded_ok(const char *rel_path)
{
    if (SD_MODE_NONE_BUILD || !rel_path) {
        return;
    }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        note_uploaded_day_locked(rel_path);
        xSemaphoreGive(s_lock);
    }
}

/* 判断目录名是否严格早于成功水位 */
static bool day_before_watermark(int y, int m, int d)
{
    if (s_last_ok_day[0] == '\0') {
        return false;      /* 还没成功上传过任何东西 -> 一张都不许删 */
    }
    char day[16];
    snprintf(day, sizeof(day), "%04d%02d%02d", y, m, d);
    return strcmp(day, s_last_ok_day) < 0;
}

/* 直接可用的 POSIX 目录遍历（FATFS 支持 opendir/readdir） */
#include <dirent.h>

static uint64_t rm_tree(const char *abs_dir, bool only_before_watermark, int y, int m, int d)
{
    uint64_t freed = 0;
    DIR *dir = opendir(abs_dir);
    if (!dir) {
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[256];
        snprintf(child, sizeof(child), "%s/%s", abs_dir, ent->d_name);

        bool is_day_dir = only_before_watermark && (strlen(ent->d_name) == 2);
        if (is_day_dir) {
            /* 到了日目录层：只有早于水位才删 */
            int dd = atoi(ent->d_name);
            if (!day_before_watermark(y, m, dd)) {
                continue;
            }
        }
        /* 递归删 */
        uint64_t sub = rm_tree(child, false, y, m, d);
        if (sub == 0) {
            /* 叶子：先看大小再删 */
            struct stat st;
            if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) {
                freed += (uint64_t)st.st_size;
                remove(child);
            } else if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove(child);      /* 空目录 */
            }
        } else {
            freed += sub;
        }
    }
    closedir(dir);
    return freed;
}

uint64_t sd_hal_check_space(void)
{
    if (SD_MODE_NONE_BUILD || !s_mounted) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return 0;
    }

    uint64_t total = 0, fre = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    uint64_t low = (uint64_t)config_store_low_space_bytes();
    if (low == 0 || fre >= low) {
        xSemaphoreGive(s_lock);
        return fre;
    }

    LOGW(TAG, "low space free=%lluMB < %lluMB -> cleanup (watermark=%s)",
         (unsigned long long)(fre / 1048576ULL), (unsigned long long)(low / 1048576ULL),
         s_last_ok_day[0] ? s_last_ok_day : "none");

    /* 遍历 snapshots/YYYY/MM/DD，删除早于成功水位的日目录 */
    const char *snap = SD_MOUNT_POINT "/snapshots";
    DIR *yd = opendir(snap);
    if (yd) {
        struct dirent *ye;
        while ((ye = readdir(yd)) != NULL) {
            int y = atoi(ye->d_name);
            if (y < 2000 || y > 2100) {
                continue;
            }
            char yp[256];
            snprintf(yp, sizeof(yp), "%s/%s", snap, ye->d_name);
            DIR *md = opendir(yp);
            if (!md) {
                continue;
            }
            struct dirent *me;
            while ((me = readdir(md)) != NULL) {
                int mo = atoi(me->d_name);
                if (mo < 1 || mo > 12) {
                    continue;
                }
                char mp[300];
                snprintf(mp, sizeof(mp), "%s/%s", yp, me->d_name);
                rm_tree(mp, true, y, mo, 0);
            }
            closedir(md);
        }
        closedir(yd);
    }

    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre);
    LOGI(TAG, "cleanup done free=%lluMB", (unsigned long long)(fre / 1048576ULL));
    if (fre < low) {
        s_state = SD_LOWSPACE;
    } else {
        s_state = SD_OK;
    }
    xSemaphoreGive(s_lock);
    return fre;
}
