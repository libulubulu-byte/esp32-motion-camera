/**
 * @file logger.c
 * @brief 串口日志 + 内存环形日志（供 GET /api/log）
 *
 * 输出格式（任务书第 6 节）：
 *   [BOOT] ESP32-S3 Snapshot Kit v0.1.1 sensor=OV2640(0x2642) sd_mode=OPTIONAL
 *   [SD  ] mount ok total=15193MB free=15001MB cid=27504853...
 * 即 "[" + TAG(左对齐 4 字符) + "]" + 空格 + msg。
 * W/E 级别追加到 TAG 内，形如 [SD !] / [SD E]，便于 grep 告警。
 */

#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

/* 本模块刻意不使用 ESP_LOGI/ESP_LOGW：那会在我们自己的 "[TAG ] msg" 前
 * 再套一层 "I (123) logger: " 双前缀。全部输出都走 logger_printf(),
 * 因此这里不需要 TAG 变量。 */

/* ------------------------------ 内存环形日志 ------------------------------ */
static char             s_ring[LOG_RING_LINES][LOG_RING_LINE_SZ];
static int              s_ring_head;      /* 下一个写入位置 */
static int              s_ring_count;     /* 已写入行数（<= LOG_RING_LINES） */
static SemaphoreHandle_t s_lock;
/* ★ 串口写锁：stdout(UART0) 是所有任务共用的，fputs/fputc 各自不是原子的。
 *   之前不加锁，导致本模块的 [PIPE ]/[SYS  ] 与 esp32-camera 的
 *   ESP_LOGE("cam_hal", ...) 两组输出在字节层交错，串口上出现乱码拼行
 *   （典型：把 [SYS  ] 行里的 "buffer" 切成 "b" + "cam_hal: ..."）。
 *   注意这把锁只保护"拼行 -> 一次写"，不保护格式化，开销极小。 */
static SemaphoreHandle_t s_uart_lock;

#ifdef CONFIG_ESP_CONSOLE_UART
/* ★ 接管 ESP-IDF 原生日志的输出通道。
 *   问题：ESP_LOGx 内部用的是 vprintf → stdout 的 stdio 锁，我们的 fwrite 走的也是同一把锁 ——
 *   这层其实已经互斥了。但相机驱动(cam_hal/sccb)会在**任务和中断/DMA 回调**两种上下文里打日志，
 *   而 FreeRTOS 的 mutex 不允许在 ISR 里 take，SPI Flash 操作时又可能重入，
 *   于是两边的行就会在字节层互相打断，典型是：
 *       "wifi:Init data frame dycam_hal: NO-SOnamic rx buffer num: 32"
 *   这里用 esp_log_set_vprintf 把原生日志重定向到我们自己的 vprintf 钩子，
 *   与 logger_printf 共用同一把 s_uart_lock，保证"整行一次写"。
 *   钩子里**绝不**调用 logger_printf，避免递归。 */
static int logger_vprintf(const char *fmt, va_list ap)
{
    char buf[LOG_RING_LINE_SZ];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);
    if (n <= 0) {
        return n;
    }

    bool locked = (s_uart_lock != NULL) &&
                  (xSemaphoreTake(s_uart_lock, pdMS_TO_TICKS(20)) == pdTRUE);
    fputs(buf, stdout);
    fflush(stdout);
    if (locked) {
        xSemaphoreGive(s_uart_lock);
    }
    return n;
}
#endif /* CONFIG_ESP_CONSOLE_UART */

void logger_init(void)
{
    if (s_lock == NULL) {
        /* 用 mutex 而不是临界区：日志可能带参数格式化，耗时不可控，不适合关中断 */
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_uart_lock == NULL) {
        s_uart_lock = xSemaphoreCreateMutex();
    }
#ifdef CONFIG_ESP_CONSOLE_UART
    /* 把 esp_log 的输出也纳入同一把锁，消除与相机/WiFi 的字节级交错 */
    esp_log_set_vprintf(logger_vprintf);
#endif
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head = 0;
    s_ring_count = 0;
}

/* 必须在持锁状态下调用 */
static void ring_push(const char *line)
{
    strlcpy(s_ring[s_ring_head], line, LOG_RING_LINE_SZ);
    s_ring_head = (s_ring_head + 1) % LOG_RING_LINES;
    if (s_ring_count < LOG_RING_LINES) {
        s_ring_count++;
    }
}

void logger_printf(char level, const char *tag, const char *fmt, ...)
{
    char body[192];
    char line[LOG_RING_LINE_SZ];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* 组装 "[TAG ] msg"：TAG 左对齐补空格到 4 字符，'W'/'E' 额外加一个等级符号，
     * 'I' 则是空格 —— 这样所有日志的 "]" 都对齐在第 7 列，parse_log.py 可以直接切列。 */
    const char mark = (level == 'E') ? 'E' : (level == 'W') ? 'W' : ' ';
    /* body 最长 191 字节，line 只有 160；这里显式用 "%.*s" 限长，避免
     * -Werror=format-truncation 把编译打断（截断是预期行为，不是缺陷） */
    snprintf(line, sizeof(line), "[%-4.4s%c] %.130s",
             tag ? tag : "SYS", mark, body);

    /* 1) 存内存环形（给 /api/log） */
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        ring_push(line);
        xSemaphoreGive(s_lock);
    }

    /* 2) 出串口。直接写到 stdout（UART0）。ESP_LOG 会把我们的方括号再包一层，
     *    所以这里不用 ESP_LOGI，避免出现 "I (123) logger: [BOOT] ..." 的双前缀。
     *
     * ★ 整行必须"一次写入 + 持锁"：
     *   - 持锁：防止与其他任务的 UART 输出字节级交错；
     *   - 一次写入：把 "line\n" 先拼成一个缓冲再一次 fwrite，
     *     避免 fputs+fputc 两步之间被抢占（即使持锁，两步也更容易和
     *     阻塞在 UART 驱动里的其他写者交叉）。
     *   锁拿不到时（极罕见）直接输出，宁可偶尔乱序也不能丢日志。 */
    char out[LOG_RING_LINE_SZ + 2];
    int n = snprintf(out, sizeof(out), "%s\n", line);
    if (n > 0) {
        bool locked = (s_uart_lock != NULL) &&
                      (xSemaphoreTake(s_uart_lock, pdMS_TO_TICKS(20)) == pdTRUE);
        fwrite(out, 1, (size_t)n < sizeof(out) ? (size_t)n : sizeof(out) - 1, stdout);
        fflush(stdout);
        if (locked) {
            xSemaphoreGive(s_uart_lock);
        }
    }
}

size_t logger_snapshot(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    size_t used = 0;
    int n = s_ring_count;
    /* 从最旧的一行开始遍历 */
    int start = (s_ring_count < LOG_RING_LINES) ? 0 : s_ring_head;
    for (int i = 0; i < n; i++) {
        const char *l = s_ring[(start + i) % LOG_RING_LINES];
        size_t need = strlen(l) + 1;
        if (used + need >= buf_sz) {
            break;
        }
        memcpy(buf + used, l, need - 1);
        used += need - 1;
        buf[used++] = '\n';
    }
    buf[used] = '\0';
    xSemaphoreGive(s_lock);
    return used;
}

/* JSON 字符串转义：只处理必须转义的那几个字符 */
static size_t json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 7 < dst_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r') {
            /* 串口行尾的 \r 直接丢掉 */
        } else if (c == '\t') {
            dst[o++] = '\\';
            dst[o++] = 't';
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

size_t logger_dump_json(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz < 8) {
        return 0;
    }
    size_t o = 0;
    buf[o++] = '[';
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        int n = s_ring_count;
        int start = (s_ring_count < LOG_RING_LINES) ? 0 : s_ring_head;
        bool first = true;
        for (int i = 0; i < n; i++) {
            char esc[LOG_RING_LINE_SZ * 2];
            json_escape(s_ring[(start + i) % LOG_RING_LINES], esc, sizeof(esc));
            size_t need = strlen(esc) + 4;
            if (o + need >= buf_sz) {
                break;
            }
            if (!first) {
                buf[o++] = ',';
            }
            first = false;
            buf[o++] = '"';
            size_t el = strlen(esc);
            memcpy(buf + o, esc, el);
            o += el;
            buf[o++] = '"';
        }
        xSemaphoreGive(s_lock);
    }
    if (o + 2 >= buf_sz) {
        o = buf_sz - 2;
    }
    buf[o++] = ']';
    buf[o] = '\0';
    return o;
}
