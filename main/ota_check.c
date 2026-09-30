/**
 * @file ota_check.c
 * @brief 上电自动升级：取版本清单 -> 语义化比较 -> 高才升级
 *
 * 流程（参考 AWS_mqtt_REST_API 的 ota_update.c，按本工程结构重写）：
 *   1. APP_OTA_CHECK_URL 为空 -> 功能关闭，直接返回
 *   2. GET 清单（几百字节 JSON），超时 APP_OTA_CHECK_TIMEOUT_MS
 *   3. 从 JSON 里抠出 "version" / "url" / "md5"（不引 cJSON，格式是我们自己发的）
 *   4. 用 app_version_is_newer() 严格比较：清单版本 <= 本机 -> 跳过
 *   5. 更高 -> ota_hal_perform(url, md5, stop_stream) 下载、校验、写分区、重启
 *
 * 与参考工程的一处**有意差异**：
 *   参考工程用分步 esp_https_ota API（begin -> get_img_desc -> perform），
 *   能在写 flash 前先读镜像头里的版本再比一次，双保险；
 *   本工程沿用已有的 ota_hal（整包下到 PSRAM -> MD5 -> 写分区），
 *   好处是复用已经过验证的下载/校验路径与失败处理，不动既有代码。
 *   代价：判据只有清单里的 version。清单是我们自己发的静态文件，
 *   可信度与镜像头相当；且写分区前仍有 MD5 + esp_ota_end 的镜像校验兜底。
 *
 * 防降级：清单版本不高于本机一律跳过。所以服务器上留着旧 .bin 不会把
 * 设备刷回去 —— 反过来说，刷错版本后也无法用这个机制退回，只能手工烧。
 */

#include "ota_check.h"

#include "app_conf.h"
#include "app_version.h"
#include "config_store.h"
#include "logger.h"
#include "ota_hal.h"
#include "version.h"
#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = LOG_T_OTA;

#define MANIFEST_MAX_LEN   512

/* ------------------------------------------------------------------ */
/* 清单解析                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 从 JSON 里取字符串键值，支持 "key":"value" 与 "key": "value"
 *
 * 不为清单引入 cJSON：清单是我们自己发的一个静态文件，格式固定，
 * 而为它链接 json 组件的收益（省十行代码）小于成本（启动路径多一层依赖）。
 *
 * @return true = 取到非空值
 */
static bool manifest_get(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

/**
 * @brief GET 版本清单到 buf（栈上分配，调用方给缓冲）
 *
 * 一次性把手写的最小 HTTP client 用完即弃：清单只有几百字节，
 * 不值得复用连接，也不值得常驻一个 static 缓冲（512B 会一直占 DRAM）。
 */
static esp_err_t fetch_manifest(char *out, size_t out_sz)
{
    esp_http_client_config_t hc = {
        .url               = APP_OTA_CHECK_URL,
        .timeout_ms        = APP_OTA_CHECK_TIMEOUT_MS,
        /* 清单是公开信息，不含密钥，用内置根证书包即可。
         * 若将来换自签证书的 https 服务器，把这里改成 .cert_pem。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        LOGE(TAG, "manifest client init fail");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        /* 服务器不在线是**最常见**的情况，不算异常，日志保持一行 INFO/WARN 级 */
        LOGW(TAG, "GET %.100s failed: %s（跳过自动升级）",
             APP_OTA_CHECK_URL, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    (void)esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status < 200 || status >= 300) {
        LOGW(TAG, "manifest -> HTTP %d（跳过自动升级）", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < (int)out_sz - 1) {
        int n = esp_http_client_read(c, out + total, (int)(out_sz - 1 - total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    out[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (total == 0) {
        LOGW(TAG, "manifest is empty");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 上电检查                                                            */
/* ------------------------------------------------------------------ */

esp_err_t ota_check_on_boot(void)
{
    /* ---- 0. 功能开关：URL 留空 = 不做自动升级 ---- */
    /* 用 strlen 而不是 url[0]：宏可能被写成 "" 也可能被写成空指针字面量，
     * 这里只关心"有没有内容"。 */
    if (strlen(APP_OTA_CHECK_URL) == 0) {
        LOGI(TAG, "auto-update disabled (APP_OTA_CHECK_URL 为空)");
        return ESP_OK;
    }

    const char *running = SNAP_FW_VERSION;
    LOGI(TAG, "check version: running=%s url=%.100s", running, APP_OTA_CHECK_URL);

    /* ---- 1. 取清单 ---- */
    /* 在栈上分配：本函数只在启动路径调用一次，返回前就退出作用域。
     * 放 static 反而让这 512 字节常驻 DRAM。 */
    char manifest[MANIFEST_MAX_LEN];
    if (fetch_manifest(manifest, sizeof(manifest)) != ESP_OK) {
        return ESP_FAIL;   /* 已经在 fetch_manifest 里打过原因 */
    }
    LOGI(TAG, "manifest: %.200s", manifest);

    /* ---- 2. 解析 ---- */
    char srv_ver[32] = {0};
    char srv_url[192] = {0};
    char srv_md5[40] = {0};

    if (!manifest_get(manifest, "version", srv_ver, sizeof(srv_ver))) {
        LOGE(TAG, "manifest 缺 \"version\" 字段 -> 无法判断，跳过");
        return ESP_FAIL;
    }
    if (!manifest_get(manifest, "url", srv_url, sizeof(srv_url))) {
        LOGE(TAG, "manifest 缺 \"url\" 字段 -> 跳过");
        return ESP_FAIL;
    }
    /* md5 可选：取不到就跳过本地校验（ota_hal 仍会做镜像头校验） */
    (void)manifest_get(manifest, "md5", srv_md5, sizeof(srv_md5));

    /* ---- 2b. 格式守卫：版本必须是纯 数字[.数字]… ----
     *
     * 这不是多余的谨慎。app_version_parse() 会把任意字符串里**能读到的数字**
     * 当作版本段，于是 "676dc691-dirty" 被解析成 676.0.0 —— 远大于 "0.1.1"。
     * 后果不是"误升一次"，而是**每次上电都判定有新版本**：下载同一份镜像、
     * 写入、重启，新镜像的版本依然是同一个字符串，于是再来一遍，无限刷机。
     * 这类事故表现成"设备不停重启"，很难联想到版本号格式。
     *
     * 用 IDF 默认的 git describe 版本（PROJECT_VER 未定义时）正好会产生这种
     * 字符串；本工程已在顶层 CMakeLists.txt 里把镜像头版本对齐到 version.h，
     * 这里是第二道防线：只接受形如 0.1.1 / 1.2 / 3 的清单版本。
     */
    {
        const char *p = srv_ver;
        bool ok = (*p != '\0');
        int fields = 0;
        while (ok && *p) {
            if (!isdigit((unsigned char)*p)) {
                ok = false;            /* 出现非数字非点 -> 拒绝 */
                break;
            }
            while (isdigit((unsigned char)*p)) {
                p++;
            }
            fields++;
            if (*p == '.') {
                /* 点号后面**必须**紧跟数字："0.1." 这种结尾点要拒掉。
                 * 漏掉这个检查时 "0.1." 会被接受，并解析成 0.1.0 ——
                 * 不会导致刷机循环（0.1.0 < 0.1.1），但说明守卫在放行畸形输入。 */
                if (!isdigit((unsigned char)p[1])) {
                    ok = false;
                    break;
                }
                p++;
            } else if (*p != '\0') {
                ok = false;            /* 数字后面既不是点也不是结尾 */
            }
        }
        if (!ok || fields > 4) {
            LOGE(TAG, "清单 version=\"%s\" 不是 数字[.数字] 形式 -> 拒绝比较"
                      "（若放行会导致每次上电都升级，无限刷机）", srv_ver);
            return ESP_FAIL;
        }
    }

    /* ---- 3. 比较：只升不降 ---- */
    if (!app_version_is_newer(srv_ver, running)) {
        int cmp = app_version_cmp(srv_ver, running);
        LOGI(TAG, "up to date: server=%s running=%s (%s)",
             srv_ver, running,
             (cmp == 0) ? "相同" : "服务器更低，拒绝降级");
        return ESP_OK;
    }

    LOGI(TAG, "new firmware: %s -> %s%s", running, srv_ver,
         srv_md5[0] ? " (md5 已提供)" : " (无 md5，仅镜像头校验)");

    /* ---- 4. 交给 ota_hal 执行 ----
     * prepare 回调传 web_server_stream_stop：本函数跑在启动路径上，
     * 此刻通常没人看流；但若将来有人把它改成"运行中也可调用"，
     * 这一步能保证 MJPEG 不会边推流边被 OTA 打断。
     * （CONFIG_SNAP_ENABLE_STREAM 关闭时它是空函数，无副作用。） */
    esp_err_t err = ota_hal_perform(srv_url, srv_md5[0] ? srv_md5 : NULL,
                                    web_server_stream_stop);
    if (err != ESP_OK) {
        /* ota_hal 已保证：失败不重启、不写分区。这里只补一条"设备仍可用"的说明，
         * 免得现场看到 [OTA] failed 以为设备坏了。 */
        LOGE(TAG, "auto-update failed: %s -> 继续以 v%s 运行", ota_hal_last_error(), running);
        return err;
    }

    /* 走到这里说明升级成功，ota_hal 内部已 esp_restart()，通常不会返回 */
    LOGI(TAG, "auto-update ok, rebooting...");
    return ESP_OK;
}
