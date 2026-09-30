/**
 * @file upload_queue.h
 * @brief 上传队列：path 模式（SD 有文件）/ buffer 模式（PSRAM 里有 JPEG）
 *
 * 任务书 5.4 / 5.6：
 *  - SD_OK  : has_file=true，只存相对路径，不复制 JPEG（省 PSRAM）
 *  - 其余   : has_file=false，存 PSRAM 指针，队列项被丢弃/消费后必须 free
 *  - 队列满  : 按 queue_policy(drop_new / drop_oldest) 处理，并释放被丢弃项内存
 *
 * 唯一内存所有权规则：
 *   push 之后内存归队列所有；pop 之后归取走者所有（必须调 upload_queue_free_item）；
 *   队列销毁/项被丢弃时队列负责 free。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 触发来源（meta 里要写）
 *
 * ★ 新增值只能**追加在末尾**：这些枚举会写进 meta.trigger 字符串，
 *   中间插入会让既有取值的含义发生位移。
 */
typedef enum {
    TRIG_SRC_PIR = 0,
    TRIG_SRC_BUTTON,
    TRIG_SRC_REMOTE_HTTP,
    TRIG_SRC_REMOTE_MQTT,
    TRIG_SRC_TIMER,
    TRIG_SRC_TELEGRAM,      /**< Telegram /snap 命令（与 remote_mqtt 区分开） */
} trig_src_t;

const char *trig_src_str(trig_src_t s);

/** meta_json 里的全部字段（任务书 5.4 规定的 13 项） */
typedef struct {
    char     device_id[32];
    uint32_t event_id;
    int64_t  ts;              /**< epoch 秒；time_valid=false 时为 0 */
    bool     time_valid;
    uint32_t boot_ms;
    trig_src_t trigger;
    int      rssi;
    uint32_t free_heap;
    uint32_t free_psram;
    const char *sd_state;
    int      flash_used;      /**< 0/1，本次是否开过补光 */
    size_t   jpeg_len;
    const char *sensor;
} meta_info_t;

typedef struct {
    uint32_t   event_id;
    bool       has_file;      /**< true=path 模式 */
    char       rel_path[200]; /**< has_file=true：相对 /sdcard 的路径 */
    uint8_t   *buf;           /**< has_file=false：PSRAM 里的 JPEG（队列拥有） */
    size_t     len;
    char      *meta_json;     /**< 已序列化的 meta（PSRAM 或内部 RAM，队列拥有） */
    uint32_t   attempts;
} queue_item_t;

/** 队列位模式："path" / "buffer" */
const char *upload_queue_mode_str(void);

/** 初始化（读 config 的 max_queue_len / queue_policy，创建上传任务） */
esp_err_t upload_queue_init(void);

/**
 * @brief 入队
 * @param has_file  true=只存 rel_path（SD 模式）；false=接管 buf 的所有权
 * @param buf       has_file=false 时必填；入队成功后由队列负责释放（含失败时释放）
 * @param meta      meta 信息（函数内部序列化成 JSON）
 * @param out_id    返回分配的事件 id，可为 NULL
 * @retval ESP_ERR_NO_MEM 队列满且策略为 drop_new（buf 已被本函数释放）
 */
esp_err_t upload_queue_push(bool has_file, const char *rel_path,
                            uint8_t *buf, size_t len,
                            const meta_info_t *meta, uint32_t *out_id);

/** 队列长度 */
int upload_queue_len(void);

/** 队列当前占用字节数（buffer 模式才是真占用，path 模式按 len 计入统计） */
size_t upload_queue_bytes(void);

/** 累计成功/失败次数 */
uint32_t upload_queue_ok_count(void);
uint32_t upload_queue_fail_count(void);

/** 累计因队列满被丢弃的次数 */
uint32_t upload_queue_drop_count(void);

/**
 * @brief 累计"所有上传通道均未配置"而未被上送的次数
 *
 * 与 upload_queue_fail_count() 区分开：这不是上传失败（网络/服务端错误），
 * 而是部署时没配 http_url / telegram / mqtt 任一出口。SD 模式下照片仍在卡上，
 * 只有内存模式才会真正丢弃。
 */
uint32_t upload_queue_no_channel_count(void);

/** 最近一次错误描述（给 /api/status 的 last_error） */
const char *upload_queue_last_error(void);

/**
 * @brief 内存环形事件表（最近 50 条 meta），供 GET /api/events
 * @return 写入 buf 的字节数（JSON 数组）
 */
size_t upload_queue_events_json(char *buf, size_t buf_sz);

/** 手动重试：把队列里所有项立刻唤醒一次（POST /api/snapshot 后无需，调试用） */
void upload_queue_kick(void);

#ifdef __cplusplus
}
#endif
