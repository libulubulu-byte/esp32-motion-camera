/**
 * @file logger.h
 * @brief 串口日志规范（任务书第 6 节）
 *
 * 统一 TAG 宽度为 6 字符，形如：
 *   [BOOT] ...
 *   [CAM ] ...
 *   [SD  ] ...
 * 保证解析脚本 tools/parse_log.py 可以用 `^\[(BOOT|CAM|SD ...)\]` 直接切列。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------- TAG 定义 ------------------------------- */
#define LOG_T_BOOT    "BOOT"   /**< 启动横幅、芯片信息、就绪    */
#define LOG_T_CFG     "CFG"    /**< 配置读写                    */
#define LOG_T_WIFI    "WIFI"   /**< STA/AP/重连                 */
#define LOG_T_CAM     "CAM"    /**< 摄像头 init / grab / return */
#define LOG_T_SD      "SD"     /**< SD 状态机                   */
#define LOG_T_TRIG    "TRIG"   /**< 触发状态机                  */
#define LOG_T_PIPE    "PIPE"   /**< 拍照管道 / 分流             */
#define LOG_T_Q       "Q"      /**< 上传队列                    */
#define LOG_T_UPLOAD  "UPLOAD" /**< HTTP 上传                   */
#define LOG_T_TG      "TG"     /**< Telegram                    */
#define LOG_T_MQTT    "MQTT"   /**< MQTT                        */
#define LOG_T_WEB     "WEB"    /**< web server                  */
#define LOG_T_OTA     "OTA"    /**< OTA                         */
#define LOG_T_SYS     "SYS"    /**< 时间同步 / 重启 / 杂项       */
#define LOG_T_LED     "LED"    /**< LED / 按键                  */

/* `[TAG ]` 已由宏拼接，调用时 msg 不要重复带 tag 前缀 */
#define LOGI(tag, fmt, ...) logger_printf('I', tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) logger_printf('W', tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) logger_printf('E', tag, fmt, ##__VA_ARGS__)

/**
 * @brief 初始化日志（banner 之前的最后一步 / 第一步）
 * @note  必须在任何 LOGx 之前调用。内部是幂等的。
 */
void logger_init(void);

/**
 * @brief 按任务书格式输出一行日志
 * @param level 'I' / 'W' / 'E'，'W'/'E' 会在方括号里额外标注等级
 * @param tag   4 字符 TAG（不足补空格到 4 位）
 */
void logger_printf(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/**
 * @brief 把最近的日志留存到内存环形缓冲，供 GET /api/log 读取
 *
 * 任务书 5.7 要求 /api/log 返回日志。直接读 UART 环形缓冲不可行，
 * 所以这里另存一份"轻量环形记录"（只存格式化后的整行，最多 LOG_RING_LINES 行）。
 */
#define LOG_RING_LINES   80
#define LOG_RING_LINE_SZ 160

/** 取日志快照，返回实际写入的行数。buf 需 >= LOG_RING_LINES*LOG_RING_LINE_SZ */
size_t logger_snapshot(char *buf, size_t buf_sz);

/** 供 /api/log 用：把环形缓冲以 JSON 数组字符串写出 */
size_t logger_dump_json(char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif
