/**
 * @file app_conf.h
 * @brief ★ 部署配置：接单 / 交付时**只改这一个文件** ★
 *
 * 这里定义的是**编译期默认值**，生效条件是「设备还没有 /config.json」：
 *   - 全新芯片第一次上电
 *   - 刚做过恢复出厂（长按按键 5s 或 POST /api/factory_reset）
 *
 * 一旦用 /api/config 或配网页改过任何一项，配置就会落盘到 littlefs 的
 * /config.json，**之后以设备 flash 里的值为准** —— 运行时配置优先级更高。
 *
 * 所以改了本文件后要看到效果，需要：① 重新烧录 ② 恢复出厂。
 *
 * ⚠️ 安全提醒（与工程既定约定的冲突，明确写在这里）
 *   本工程其它地方一律「禁止硬编码密钥」（见 config_store.c 文件头 0.8 节）。
 *   这个文件是**唯一例外** —— 为了「一客户一份、改一处就烧」的交付效率。
 *
 *   当前本文件可能承载的密钥（全部留空 = 不启用）：
 *     · APP_MQTT_PASS           MQTT 口令
 *     · APP_HTTP_HEADER_VALUE   上传鉴权头
 *     · APP_TELEGRAM_TOKEN      Telegram bot token（敏感度最高）
 *
 *   因此请注意：
 *     · **不要提交到公开仓库**（建议加进 .gitignore）
 *     · 交付结束后把口令清空
 *   如果你更看重安全，把这些留空，改为交付时用 /api/config 写入。
 *
 * 本文件同时承载**非密钥**的部署参数（这些可以正常提交）：
 *   · APP_MQTT_HOST / APP_MQTT_PORT / APP_MQTT_TOPIC_PREFIX
 *   · APP_HTTP_URL
 *   · APP_WIFI_SSID / APP_WIFI_PASS
 *   · APP_OTA_CHECK_URL          上电自动升级的清单地址
 */
#pragma once

/* ================================ MQTT ================================ */

/* broker 地址与端口分开填，代码会拼成 mqtt://<host>:<port>
 * 例：192.168.0.101 / 1883
 * 也支持填域名，例如 "test.mosquitto.org" */
#define APP_MQTT_HOST       "192.168.137.1"
#define APP_MQTT_PORT       1883

/* 认证：两个都留空 = 匿名连接
 * ⚠️ 若 broker 侧是 allow_anonymous false（推荐），匿名会被直接拒绝 */
#define APP_MQTT_USER       "snapcam"
#define APP_MQTT_PASS       "snap-7Kq2mXv9Lp4R"

/**
 * 上报主题前缀。
 *
 * 最终主题 = "<前缀>/<12 位 MAC 大写十六进制>/event"
 *   本机示例：esp32S3_CAM/288485922F44/event
 *
 * 订阅单个设备：esp32S3_CAM/288485922F44/#
 * 订阅全部设备：esp32S3_CAM/+/event
 */
#define APP_MQTT_TOPIC_PREFIX   "esp32S3_CAM"

/* ============================ HTTP 上传（可选） ============================
 * 留空 = 不做 HTTP 上传（与原来的默认行为一致）。
 * 填完整 URL，例如 "http://192.168.0.101:8080/upload"。
 * 设备会以 multipart/form-data 把 meta(JSON) + file(JPEG) 发过去。
 *
 * ★ 2026-09-27 起默认指向本机的**收照片**服务（tools/recv_server.py，端口 8080），
 *   这样新芯片/恢复出厂后开箱就能上报，不必先手填配网页。
 *
 * ⚠️ 别把这里的端口和 APP_OTA_CHECK_URL 搞混（详见 docs/debug_notes.md §4）：
 *      8080 = tools/recv_server.py        收照片（就是本项）
 *      8081 = rest_mock_server/server.py  升级清单 + /firmware/（APP_OTA_CHECK_URL）
 *   两者分开是刻意的：recv_server.py 对**所有** GET 路径都回 {"server":"alive",...}，
 *   一旦它抢答了 /ota.json，设备就会报 "manifest 缺 version 字段"，看着像 OTA 坏了。
 *
 *   ⚠️ 与 APP_MQTT_HOST 共用同一个 IP：换现场时**一起改**，
 *      否则会出现"MQTT 通了但 HTTP 401/超时"这类半通状态。
 *   只想走 MQTT、不要 HTTP 的话，把 upload_mode 设为 mqtt（或 telegram），
 *   或直接在配网页把 http_url 清空。
 */
/* ⚠️★ 2026-09-28：本项**必须与 APP_MQTT_HOST / APP_OTA_CHECK_URL 同一张网卡**。
 *   现网设备挂在移动热点下（192.168.137.x），只保证能到网关 192.168.137.1；
 *   而这里曾长期留着有线网段 192.168.0.101，于是每张照片都先刷 4 条
 *       [UPLOW] http fail attempt=3 err=perform_fail_0x7004   ← ESP_ERR_HTTP_CONNECT
 *   再靠 Telegram 兜底成功。看着"能收到照片"，实际 HTTP 通道全程空转。
 *   默认值已设 upload_mode=telegram，本项留空即可彻底关掉 HTTP；
 *   要重新启用 HTTP，先把 IP 改成设备真正够得到的那个（见下方 OTA 段的完整说明）。 */
#define APP_HTTP_URL        ""

/* 上传时附带的鉴权头（multipart 请求头），用于服务端识别设备来源。
 * 留空 = 不带该头。key 留空时用工程内建的 "X-Device-Key"。
 * ⚠️ value 是密钥，与 MQTT 口令同级 —— 同样不要提交到公开仓库。 */
#define APP_HTTP_HEADER_KEY     ""
#define APP_HTTP_HEADER_VALUE   ""

/* ========================== Telegram 机器人（可选） ==========================
 * 留空 = 不上报 Telegram。
 *   token   ：BotFather 给的形如 "123456789:AAE..." 的串
 *   chat_id ：目标会话 ID（个人/群组均可，形如 "-1001234567890"）
 *
 * ★ 两个必须**同时非空**才生效：只填 token 时，发送侧与命令轮询都会直接跳过
 *   （日志：[CFG W] upload_mode=both 但 telegram token/chat_id 不完整 -> Telegram 跳过）。
 *   upload_mode 还需为 telegram 或 both（本工程默认就是 both）。
 *
 * 【chat_id 怎么拿】三步：
 *   1) 在 Telegram 里搜到你的 bot，点 START（或随便发一句话）；
 *   2) 浏览器打开 https://api.telegram.org/bot<你的token>/getUpdates
 *      （★ 不要带 offset 参数，否则会把这批消息"消费"掉）；
 *   3) 在返回 JSON 里取 result[].message.chat.id —— 正数=私聊，负数=群组，
 *      连负号一起原样填到下面。
 *   ⚠️ token 里冒号前面那串（bot 自己的 id）**不是** chat_id，填错会一直 skip。
 *
 * ⚠️ token 能直接操作你的 bot（读消息、改配置、发消息），敏感度高于 MQTT 口令。
 *    本文件是工程里唯一的"密钥硬编码"例外（见文件头说明），
 *    交付结束后建议清空，或用 /api/config 改为运行时注入。
 *
 * ⚠️★ 写在这里的值会被**编进 .bin**（`strings` 就能提取出来），且换 token 必须重烧，
 *    已用过的设备还得恢复出厂才会生效 —— 只在"批量交付、一客户一份、开箱即用"
 *    时才值得这么做。单台自用建议改用运行时注入（配网页或 POST /api/config）。
 */
/* ⚠️★ 下面两行是**本机调试用的默认值**（bot @libulubulu_bot，chat_id 为私人会话）。
 *    ★★ 交付 / 出厂前**必须清空成 ""**，否则：
 *       · 客户的设备会把照片发到**我的** Telegram 里；
 *       · 我的 token 会被编进客户的固件（`strings` 一下就能扒出来）。
 *    清空后由客户用自己的 bot：配网页填，或 POST /api/config。
 *    交付前自查一条命令（两行都应输出 ""）：
 *      Select-String main\app_conf.h -Pattern 'APP_TELEGRAM'
 *    ⚠️ 同一批交付还要一起清：APP_MQTT_PASS、APP_HTTP_HEADER_VALUE（见文件头的密钥约定）。 */
#define APP_TELEGRAM_TOKEN      "8800291160:AAEe26RhJaZHjbPDtTHOWIxafq314CeV9uo"
#define APP_TELEGRAM_CHAT_ID    "7960301969"   /* 私聊 chat.id；群组是负数，连负号一起填 */

/* ============================== Wi-Fi（可选） ==============================
 * 留空 = 空值，由用户在现场用配网页填写（推荐，因为各现场 SSID/密码不同）。
 *
 * 什么时候才在这里填？
 *   · 多个现场共用同一个 SSID/密码，且不想逐台配；
 *   · 或者产线首检要直接联网，不方便手配。
 *
 * ⚠️ 填了之后，全批固件都会带上这个密码：固件泄露 = 现场 Wi-Fi 泄露。
 *    另外注意：设备**烧录后直接就能连上**，会跳过 AP 配网流程，
 *    现场想改网络只能靠按键进 AP 模式或恢复出厂。 */
#define APP_WIFI_SSID       "HONOR"
#define APP_WIFI_PASS       "12345678"

/* ====================== 上电自动升级（版本检查，可选） ======================
 *
 * 机制：设备**每次上电**（Wi-Fi 连上后）GET 这个 URL 拿一份版本清单，
 *       若清单里的版本**比本机高**，就自动下载升级并重启；否则什么都不做。
 *
 * 清单是一个静态 JSON 文件，格式（多余的键会被忽略）：
 *   {
 *     "version": "0.1.2",
 *     "url":     "http://192.168.0.101:8081/firmware/esp32s3_snapshot_kit.bin",
 *     "md5":     "6321a4c8..."        ← 可省略；有则做 MD5 预校验
 *   }
 *
 * ⚠️ 留空字符串 = **关闭**上电自动升级（手动 POST /api/ota 推 URL 仍可用）。
 *
 * ★ 关于端口：这里用 **8081**（2026-09-27 从 8080 改过来）。
 *   为什么换：8080 被 tools/recv_server.py 那个极简接收服务器占着，它对
 *   **所有** GET 路径都回 {"server":"alive",...}，于是设备拿到一份没有
 *   "version" 字段的"清单"，日志打
 *       [OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过
 *   看起来像"OTA 坏了"，实际是端口被别的程序抢了。
 *   改到 8081 后两者互不干扰：照片上传继续走 8080（recv_server.py），
 *   升级清单走 8081（rest_mock_server/server.py）。
 *
 *   启动 8081 的方法：直接双击 rest_mock_server\start.bat 即可
 *   （脚本里已写死 set "PORT=8081"，2026-09-27 同步改过；若手工起，先
 *     set PORT=8081 再 python server.py）。
 *   一条命令跑通"停旧服务 → 构建 → 重启服务 → 自检"：run_all.bat
 *   （加 -nobuild 则跳过构建，只重启服务）。
 *
 *   ⚠️ server.py 的 /ota.json 用 request.host_url 拼下载地址 —— 这里有个
 *   很隐蔽的坑：谁用 localhost / 127.0.0.1 打开过一次清单，url 就会被写成
 *   127.0.0.1，设备于是去连**它自己**的 127.0.0.1，下载必然超时
 *   （而清单内容看起来完全正常，极易误判）。
 *   server.py 已做回环地址自动改写为局域网 IP，详见 docs/debug_notes.md §5.3。
 *
 * ⚠️ 若你把 server.py 的 PORT 改成别的值，这里要同步改。
 *
 * 关于"只升不降"：清单版本 <= 本机版本时一律跳过，所以服务器上放着旧固件
 * 不会导致回滚 —— 但也意味着**刷错版本后无法靠这个机制退回去**，只能手工烧。
 *
 * ★ 版本号必须与镜像头一致：
 *   "本机版本"取自 version.h 的 SNAP_FW_VERSION；
 *   清单里的 version 由 server.py 从 .bin 的 esp_app_desc_t 里读出。
 *   顶层 CMakeLists.txt 已把镜像头版本强制对齐到 version.h（不再让 IDF 用
 *   git describe 填），所以两者天然相同。**不要把 version.h 改成非数字版本**
 *   （如 0.1.1-rc1 或含 git 短哈希），否则比较结果不可预期、可能每次上电都升级。
 */
/* ★ 2026-09-27 从 192.168.0.101 改成 192.168.137.1 —— 换成"设备真正够得到的那张网卡"。
 *
 * 本机同时有多个 IPv4：以太网 192.168.0.101、**移动热点 192.168.137.1**、
 * 代理虚拟口 198.18.0.1。设备（192.168.137.174）是挂在热点下面的，
 * **只保证能到 192.168.137.1**（它就是热点网关 = 本机）。
 *
 * 实测（同一天）：用 192.168.0.101 手动推 OTA 时设备报
 *     [OTA E] download failed: ... err=0x7002（ESP_ERR_HTTP_CONNECT）
 * 而服务端**连 /firmware 的请求都没收到** —— 属于"偶尔能通"的那种，最难查。
 * 换成 192.168.137.1 后一次成功。
 *
 * 判据：**看设备自己的 /api/config** —— http_url / mqtt_uri 用的是哪个 IP，
 * OTA 就用哪个（本工程现场是 192.168.137.1），三者保持一致最省事。
 */
#define APP_OTA_CHECK_URL   "http://192.168.137.1:8081/ota.json"

/* 取清单的超时（毫秒）。清单只有几百字节，无需太长；
 * 太长会在服务器不在线时拖慢启动（不过本工程把检查放在启动末尾，
 * 且失败只告警不阻断，所以这里的长短只影响日志出现时机）。*/
#define APP_OTA_CHECK_TIMEOUT_MS  15000
