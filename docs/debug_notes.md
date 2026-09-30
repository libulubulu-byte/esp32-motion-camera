# 调试记录与避坑总结

> 本页只写**在本机真机实测复现过、且已定位**的内容（与 [`troubleshooting.md`](troubleshooting.md) 同一约定）。
>
> **与 `troubleshooting.md` 的分工**：
> - `troubleshooting.md` = 现象 → 根因 的**速查表**（编译期 / 传感器 / SD / 运行期 / 构建脚本）
> - **本页** = **OTA 升级 / 清单服务器 / 端口约定 / 版本号链 / 内嵌网页 / 串口工具链**
>   这几块的完整链路 + 逐条踩坑记录 + 可复制的调试命令
>
> 记录时点：**2026-09-27**，固件版本 `0.1.0` → `0.1.2`，真机 MAC `28:84:85:92:2F:44`（`device_id=cam-2f44`）。
> 追加：**2026-09-28**，按键改 BOOT0 + 短按人脸识别（见 §0）。

---

## 0. ★★ 2026-09-28：人脸检测链路原来**从来没编译过**（两个真 bug）

需求是"按键改 BOOT0、短按进入人脸识别"。改完发现 **`short 按 = 人脸识别` 根本不可能生效** ——
因为人脸检测那条路一直是关的，而关着的原因不是策略，是**它编译不过**。两个 bug 都已在本次修掉：

### bug 1：`human_detect.c` 用 C 编译器编 C++ 代码

现象（打开 `CONFIG_SNAP_ENABLE_HUMAN_DETECT=y` 后立刻暴露）：

```
main/human_detect.c:60: #include "human_face_detect.hpp"
  -> dl_define.hpp:5:10: fatal error: string: No such file or directory
    5 | #include <string>
```

根因：该文件是**纯 C++** —— `#include "human_face_detect.hpp"`、`new HumanFaceDetect(...)`、
`std::list<dl::detect::result_t> &res`、`delete s_detector`。扩展名却是 `.c`，
GCC 按 C 处理，C 模式不会去 C++ 标准库目录找 `<string>`，直接 fatal。

**修法**：`human_detect.c` → **`human_detect.cpp`**（`main/CMakeLists.txt` 同步改），
CMake 自动换成 `g++` + `-std=gnu++2b`。
因为 `human_detect.h` 里所有对外函数都有 `extern "C"`，改名对调用方
（`snapshot_pipeline.c`）**完全透明**，C 侧链接不受 name mangling 影响。
验证：`nm build/esp32s3_snapshot_kit.elf | grep human_detect_` 有 `T human_detect_init/_run`
（`T` = 真实代码段，不是退化分支）。

### bug 2：`DL_IMAGE_PIX_TYPE_RGB565LE` 这个枚举**不存在**

```
error: 'DL_IMAGE_PIX_TYPE_RGB565LE' is not a member of 'dl::image';
       did you mean 'DL_IMAGE_PIX_TYPE_RGB565'?
```

根因：esp-dl 的 `pix_type_t` 里只有 `DL_IMAGE_PIX_TYPE_RGB565`，
**大小端不是靠 pix_type 区分，而是靠 `sw_decode_jpeg()` 的第 3 个参数 `caps`**：

- `dl_image_jpeg.hpp:17` — "caps Default to 0, decode to RGB565 in **little endian**"
- `dl_image_jpeg.cpp:25-28` — `caps & DL_IMAGE_CAP_RGB565_BIG_ENDIAN ? JPEG_PIXEL_FORMAT_RGB565_BE : ..._LE`

**修法**：`DL_IMAGE_PIX_TYPE_RGB565` + `caps` 用默认值 `0` 即为小端，
与文件头注释里说的 `JPEG_PIXEL_FORMAT_RGB565_LE` 完全等价。

### bug 3：`decode_ms / infer_ms` 少除 1000（microsecond 当 millisecond）

现象（真机第一次跑通人脸链路时）：

```
[PIPE ] 人脸检测 MISS faces=0 best=0 (decode=63053ms infer=62956ms 640x480)
[PIPE ] capture took=546ms flash=300ms grab=109ms retake=0
```

**63 秒 > 整条流水线 546ms**，明显自相矛盾。

根因：`esp_timer_get_time()` 返回**微秒**，而 `human_detect_result_t.decode_ms/infer_ms`
是**毫秒**，赋值时漏了 `/1000`：

```c
r.decode_ms = (int)(t1 - t0);        /* 错：微秒当毫秒 */
r.infer_ms  = (int)(t3 - t2);
```

**修法**：`(int)((t1 - t0) / 1000)`。单位换算只在这一处做。

> **判据**：日志里 `decode + infer` 必须**小于**同一行的 `capture took`。
> 若出现"耗时比总耗时还大"，先怀疑单位，而不是怀疑模型慢。

### bug 4：★★ RGB565 **字节序反了** -> 恒判 `faces=0`（人脸检测的核心 bug）

现象（真机，人站在镜头正前方、光照正常，短按 6 次）：

```
[PIPE ] 人脸检测 MISS faces=0 best=0 ...
[PIPE ] 非人形帧丢弃 src=button phase=primary faces=0 score=0 ...
```

**每一次**都是 MISS，一张脸都检不出 —— 与"换个姿势/补光"无关，怎么站都是 0。

根因：**喂给模型的 RGB565 字节序，和模型自己声明要的不一致**。

- 模型侧（`managed_components/espressif__human_face_detect/human_face_detect.cpp:58-59`
  ，ESP32-S3 分支）声明的是：
  ```cpp
  new dl::image::ImagePreprocessor(m_model, {0,0,0}, {1,1,1},
      DL_IMAGE_CAP_RGB_SWAP | DL_IMAGE_CAP_RGB565_BIG_ENDIAN);
  ```
  ⇒ 它要 **大端 RGB565**。
- 而 `human_detect.cpp` 原来用 `caps=0` 解码 = **小端 RG565**
  （`dl_image_jpeg.cpp:25-28`：caps 默认 0 -> `JPEG_PIXEL_FORMAT_RGB565_LE`）。

⇒ 每个像素**高低字节颠倒**、565 位域全解析错，模型输入等于彩色噪声 ⇒ 恒 0 脸。

**修法**：解码时显式 `DL_IMAGE_CAP_RGB565_BIG_ENDIAN`，与模型声明对齐。

> **判据（怎么确定是字节序，而不是亮度/距离/模型没加载）**：
> · 解码失败会打 `Failed to decode jpeg.` 并 **fail-open 照常上传**；现场是 MISS + 丢弃 ⇒ 解码成功。
> · 帧不新鲜会打 `drop stale frame` 且 age 很大；现场 `age=72/36/37ms` ⇒ 帧是新的。
> · 模型没加载会打 `模型加载失败`；现场没有 ⇒ 模型在。
> → 唯一剩下的环节就是**像素格式**，而它恰好与模型声明相反。
>
> ⚠️ 换模型时**以模型侧 `ImagePreprocessor` 的 cap 为准**，别照抄这里的值。

### 排查辅助：`best=0` 是「真的没脸」，不是解码坏了

同一次日志里 `faces=0 best=0`，需排除"JPEG 解码产出垃圾像素导致模型全零"。已核对：

- `jpeg_img_t` 定义就是 `{ void *data; size_t data_len; }`（`dl_image_define.hpp:30-33`），
  本模块的 `{ .data = jpeg, .data_len = len }` 初始化字段完全匹配 ⇒ 喂进去的输入是对的。
- 若解码失败，`sw_decode_jpeg` 会走 `dl_image_jpeg.cpp:66-71`：打
  `dl_image_jpeg: Failed to decode jpeg.` 并返回空 `img_t`；而本模块对空返回会走
  `warn_fail_open("jpeg decode failed")` 打 **fail-open**、并让照片照常上传。
  日志里**没有**这条，也**没有**上传，说明解码成功且模型确实判定无脸。

⇒ `best=0` 时正确的排查方向是**光照/正脸/距离**，不是链路。

### ★ 一条重要纪律：`sdkconfig` 优先于 `sdkconfig.defaults`

排查时踩到：`sdkconfig.defaults` 里明明写着 `CONFIG_SNAP_ENABLE_HUMAN_DETECT=y`，
但生成的 `sdkconfig` 里是 `# CONFIG_SNAP_ENABLE_HUMAN_DETECT is not set` —— **生成的赢**。
所以"改开关"要两处都改，或删掉 `sdkconfig` 重新 `idf.py reconfigure`。
只改 `defaults` 会得到"我明明开了但行为没变"的假象。

> 判据：编译产物里出现
> `[N/M] Building CXX object .../human_detect.cpp.obj`（注意 **CXX**）才算真开。

---

## 1. 一句话结论（下次出问题先看这三条）

1. **上电自动升级一辈子只在"上电那一刻"查一次**。开机时 8081 服务没起来，这次开机就**永远不会**再升，日志里只留一条
   `[OTA W] GET http://.../ota.json failed: ESP_ERR_HTTP_CONNECT` —— 极容易被误判成"OTA 功能坏了"。
   ⇒ **纪律：先起服务，再给设备上电。**
2. **8080 和 8081 是两个不同的服务器，别再认错窗口**：
   `8080 = tools/recv_server.py`（收照片，**必须**留在 8080），
   `8081 = rest_mock_server/server.py`（`/ota.json` + `/firmware/` + 仪表盘）。
   `recv_server.py` 对**所有** GET 都回 `{"server":"alive",...}`，被它抢答 `/ota.json` 就必然报
   `[OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过`。
3. **网页（`main/web/index.html`）是编译期嵌进固件的**，改完**必须重新 build + 烧录**，
   页面上不会"刷新一下就变"。

---

## 2. 工程速览

**是什么**：ESP32-S3（Freenove ESP32-S3-WROOM / N16R8，8MB Octal PSRAM + 16MB flash）的
触发式抓拍上传套件。PIR 触发 → 拍照 → 按 `upload_mode` 走 HTTP / Telegram / MQTT 上报，
照片可落 SD 卡（`/sdcard/photos/`）。同一套代码用 Kconfig 切 3 种摄像头 × 3 种 SD 模式 = 6 个固件。

**主链路（改代码前先认清它）**

```
PIR/定时/按键  →  trigger_fsm.c  →  snapshot_pipeline.c  →  camera_hal.c
                                        │                      │
                                        │                      └─ esp_camera_fb_get() / fb_return()
                                        ▼
                                  upload_queue.c（PSRAM 环形队列，SD 不可用时降级驻留）
                                        │
                    ┌───────────────────┼───────────────────┐
                    ▼                   ▼                   ▼
              upload_http.c      upload_telegram.c        mqtt_hal.c
                    └─────────── 上报成功/失败 → 事件环形表 → 网页「Events」面板
```

**关键文件**

| 文件 | 职责 | 备注 |
|---|---|---|
| `main/app_conf.h` | ★ **部署参数唯一入口**（MQTT / HTTP / 配网 SSID / OTA 清单地址） | 只在"设备还没有 `/config.json`"时生效 |
| `main/version.h` | ★ **版本号唯一来源** `SNAP_FW_VERSION` | 改版本只改这里 |
| `main/ota_check.c` | 上电检查清单、比版本、决定是否升级 | **只在上电跑一次** |
| `main/ota_hal.c` | 下载 → MD5 → 写分区 → `set_boot_partition` → 重启 | 被动，等一个 URL |
| `main/web_server.c` | httpd + 内嵌网页 + 全部 `/api/*` | `h_index` 发 `index.html` |
| `main/web/index.html` | **配网页 + 视频页 = 同一个文件** | 编译期嵌入，见 §5.7 |
| `main/config_store.c` | littlefs `/config.json` 读写 | 运行时配置优先级高于 `app_conf.h` |
| `rest_mock_server/server.py` | 本地清单服务器（`/ota.json`、`/firmware/`） | 默认端口 **8081** |
| `tools/recv_server.py` | 本地收照片服务器 | 默认端口 **8080**，只有标准库依赖 |

---

## 3. 版本号与 OTA 全链路 ★

### 3.1 唯一的版本源

**`main/version.h` 的 `SNAP_FW_VERSION`**。格式必须是**纯数字、点分隔**（`0.1.1` / `1.2` / `1.2.3.4`）。
写 `0.1.1-rc1`、`v0.1.1`、含 git 短哈希 —— 都会被 `ota_check` 的格式守卫拒掉；
更危险的是 IDF 默认的 `git describe` 版本（形如 `676dc691-dirty`）会被解析成 `676.0.0`，
**恒大于本机版本 ⇒ 每次上电都刷同一个镜像、无限重启**。
顶层 `CMakeLists.txt` 有 `FATAL_ERROR` 守卫，写错直接编译失败。

### 3.2 四条链（一条断了就"升不了级"）

```
main/version.h  SNAP_FW_VERSION
      │  ①顶层 CMakeLists.txt 解析后设给 PROJECT_VER
      ▼
固件镜像头 esp_app_desc_t.version（绝对偏移 0x30，不是 0x2C）
      │  ②rest_mock_server/server.py 的 read_image_version() 直接读 .bin
      ▼
GET /ota.json 的 "version" 字段
      │  ③设备 ota_check.c 与自己的 SNAP_FW_VERSION 比大小
      ▼
决定要不要下载 "url"
```

- **不要手写 `ota.json`**：手填的版本号会与真实镜像脱节，结果是"版本号比真实的高"→
  设备反复下载同一个镜像刷机。`server.py` 从镜像头读，天然一致。
- `read_image_version()` 读的是**绝对偏移 `0x30`**。`esp_app_desc_t` 前面有**两个**保留字段
  （`reserv1[4]` + 一个 4 字节保留字），所以是 `0x20 + 0x10`。读 `0x2C` 会得到一串 NUL，
  静默解析成"没有版本"→ 直接关掉 OTA（源码注释见 `server.py` §"Reading the version"）。

### 3.3 分区表与"从哪个分区启动"

```
nvs       0x09000  0x6000
otadata   0x0f000  0x2000     ★ OTA 的必需品
phy_init  0x11000  0x1000
factory   0x20000  0x300000   ← idf.py flash 写这里
ota_0     0x320000 0x300000
ota_1     0x620000 0x300000
littlefs  0x920000 0x6d0000   ← /config.json
```

- **bootloader 优先看 `otadata`**：只要它有效，就按它记录的分区启动（`ota_0`/`ota_1`），
  **`factory` 只在 otadata 无效时才被选中**（日志：`Defaulting to factory image`）。
- `otadata` 缺失/损坏的后果：下载、MD5、写 `ota_0` **全部成功**，最后
  `esp_ota_set_boot_partition()` 返回 `0x105 (ESP_ERR_NOT_FOUND)`，镜像白写，
  bootloader 仍从 `factory` 启动，版本号永远不变。完整分析见 `README.md` §8.2。

### 3.4 两条升级路径（★ 行为不同，别混用）

| | 上电自动检查 | 手动 `POST /api/ota` |
|---|---|---|
| 触发 | 每次上电、Wi-Fi 拿到 IP 后**一次** | 人/脚本显式调用 |
| 版本比较 | **有**：清单版本 ≤ 本机 → 跳过（**只升不降不等**） | **无**：给什么 URL 就装什么（**可降级**） |
| 失败后 | 不再重试，本次开机作废 | 返回错误，设备继续运行 |
| 地址来源 | `APP_OTA_CHECK_URL`（`app_conf.h`），留空 = 关闭 | 调用时传 `url`/`md5` |

`/api/ota` 的两种传参（`web_server.c` `h_ota_post`）：

```
POST /api/ota?url=http://192.168.0.101:8081/firmware/xxx.bin&md5=<md5>     ← curl 一行
POST /api/ota   {"url":"http://...","md5":"..."}                            ← JSON body
```
它还会把 URL **存进 NVS 的 `ota_url`**（失败后排查看，不参与上电检查）；
`admin_key` 为空时**不校验** `X-Admin-Key`（`check_admin()` 直接放行）。

### 3.5 判据（成功 / 失败各看什么）

**成功**（按顺序出现）：

```
[OTA  ] check version: running=<本机> url=http://192.168.0.101:8081/ota.json
[OTA  ] manifest: {"version":"0.1.2","url":"http://192.168.0.101:8081/firmware/..."}
[OTA  ] newer 0.1.0 -> 0.1.2 -> 下载
...
[OTA  ] write ok part=ota_0 -> reboot in 1s to apply v0.1.0
```
重启后 `GET /api/status` → `"fw_version":"0.1.2","running_partition":"ota_0"`，
开机日志不再出现 `Defaulting to factory image`。

**失败**（三种，一眼分流）：

| 日志 | 含义 | 去查 |
|---|---|---|
| `[OTA W] GET ... failed: ESP_ERR_HTTP_CONNECT` | TCP 都没连上 | 服务没起 / 端口不对 / 防火墙 |
| `[OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过` | 拿到了 JSON 但**不是清单**（常见于被 `recv_server.py` 抢答） | §5.2 |
| 清单正常、下载也 200，但**版本号不变** | `set_boot_partition` 失败，多半是 `otadata` | §3.3 / README §8.2 |

---

## 4. 本地服务与端口约定

### 4.1 谁在哪个端口（2026-09-27 定案）

| 端口 | 程序 | 干什么 | 谁必须指向它 |
|---|---|---|---|
| **8080** | `tools/recv_server.py` | 收照片（multipart 落盘）、极简日志 | 设备配网页的 `http_url` = `http://<pc-ip>:8080/upload` |
| **8081** | `rest_mock_server/server.py` | `/ota.json` + `/firmware/*` + REST 仪表盘 | `app_conf.h` 的 `APP_OTA_CHECK_URL` |

**为什么分开**：`recv_server.py` 的 `do_GET` 对**任何**路径都回
`{"server":"alive","hint":"POST multipart to this URL"}`，设备拿它当 `/ota.json` 读，
就会报"清单缺 version"。分开端口后互不干扰。
（`app_conf.h` 的 `APP_MQTT_HOST` / `APP_HTTP_URL` / `APP_OTA_CHECK_URL` 共用同一个 IP，
**换现场三处一起改**，否则会出现"MQTT 通了但 HTTP 超时"这种半通状态。）

### 4.2 脚本职责

| 脚本 | 作用 | 端口 |
|---|---|---|
| `rest_mock_server/start.bat` | 起服务（查 IP、缺 Flask 自动装、开浏览器） | 已写死 `set "PORT=8081"` |
| `rest_mock_server/stop.bat` | 先按 `server.pid` 杀、再按端口兜底扫 | — |
| `rest_mock_server/status.bat` | 看进程 / 端口 / HTTP | — |
| `rest_mock_server/run_all.bat` | **停旧服务 → 构建（可选 `-nobuild`）→ 重启服务 → 自检** | 8081 |
| `rest_mock_server/publish_firmware.py` | 把 `build/*.bin` 拷进 `firmware/` 并算 MD5 | — |

> ★ `run_all.bat` 里有一段**很容易被误解**的保护：它会先杀掉"当前监听 `%PORT%` 的进程"，
> 再重新启动。所以**不要把它和 `recv_server.py` 混着用**——它只管 8081。

### 4.3 `/ota.json` 的形态

```json
{"version":"0.1.2",
 "url":"http://192.168.0.101:8081/firmware/esp32s3_snapshot_kit.bin",
 "md5":"bf968828b5175162084d553b52aeadbc",
 "size":1350512}
```

- `url` 由 `request.host_url` 拼出来 —— **这带来一个坑，见 §5.3**。
- 挑的是 `firmware/` 里 **mtime 最新的 `.bin`**（**不递归**，所以子目录可以当"备用版本仓库"）。
- 没有可读镜像头的 bin → 返回 500 `has no readable app image header`（宁可拒绝也不给错清单）。

### 4.4 ★ 该用哪个 IP：多网卡 + 代理时最容易踩的一处（2026-09-27 实测）

现场那台 PC 同时有 4 个可用 IPv4：

| 地址 | 网卡 | 设备能否到 |
|---|---|---|
| **192.168.137.1** | 移动热点（`本地连接* 2`） | ✅ **一定能**（它就是热点网关 = 这台 PC） |
| 192.168.0.101 | 以太网（真·局域网） | ⚠️ **偶尔能通**（要靠"同机另一个网段"绕，实测会失败） |
| 198.18.0.1 | v2cloud 代理虚拟口 | ❌ 设备永远到不了 |
| 169.254.x.x | 未拿到 DHCP 的网卡 | ❌ 无效 |

实测记录（同一天、同一个服务）：

```
用 192.168.0.101 手动推 OTA：
  [OTA  ] start url=http://192.168.0.101:8081/firmware/esp32s3_snapshot_kit.bin
  [OTA E] download failed: download fail http=0 err=0x7002     ← ESP_ERR_HTTP_CONNECT
  服务端只看到我自己的 GET /ota.json，**连 /firmware 的请求都没收到**
改用 192.168.137.1：一次成功
```

**选地址的判据（不用猜）**：看**设备自己**的 `/api/config` ——
`http_url` / `mqtt_uri` 用的是哪个 IP，OTA 就用哪个（现场三者都是 `192.168.137.1`）。
`main/app_conf.h` 的 `APP_OTA_CHECK_URL` 也已对齐到 `192.168.137.1`。

> 附带一条同源现象：`server.py` 的 `_lan_ip()` 在有代理时会先选中 `198.18.0.1`
> （默认路由的源地址是虚拟网卡），导致 `/ota.json` 里的 url 设备够不到 ——
> 已在 §5.3 修掉（过滤虚拟段 + 优先复用"设备真实用过的地址"）。

---

## 5. ★ 本次实测踩到的坑（逐条）

### 5.1 上电自动升级只查一次 ⇒ "开机时服务不在"= 这次开机永不升级

**现象**：串口只有孤零零两行，然后彻底安静，之后怎么等都不升级：

```
[OTA  ] check version: running=0.1.0 url=http://192.168.0.101:8081/ota.json
E (19187) esp-tls: [sock=48] select() timeout
[OTA W] GET http://192.168.0.101:8081/ota.json failed: ESP_ERR_HTTP_CONNECT（跳过自动升级）
```

**根因**：`ota_check_on_boot()` 只在**上电、Wi-Fi 拿到 IP 后**调用一次
（`main/ota_check.h` 明确写了"上电跑一次"），**失败不重试、也没有周期任务**。

**本次的时间账**（对得上就说明是纯时序问题）：

| 时刻 | 事件 |
|---|---|
| 19:22:21 | 设备上电（由 `/api/status` 的 `uptime_ms` 反推） |
| 19:22:40 | 唯一一次检查 → 8081 无人监听 → failure |
| **19:23:29** | 服务才被拉起来 |
| 19:29:36 | 手动 `POST /api/ota` → 立刻下载成功 |

设备开机比服务早了 **68 秒**，检查窗口错过。

**处理 / 纪律**：**先起服务（确认 `netstat` 有 `0.0.0.0:8081 LISTENING`），再复位设备。**
若已错过，不必重烧 —— 用 §6 的手动 `/api/ota` 立刻补一次即可。

**判据**：服务端 `server.log` 出现来自 `192.168.0.104`（设备）的
`GET /ota.json` + 紧随其后的 `GET /firmware/xxx.bin`。

---

### 5.2 `/ota.json` 被 `recv_server.py` 抢答 ⇒ "清单缺 version"

**现象**：

```
[OTA  ] manifest: {"server":"alive","hint":"POST multipart to this URL"}
[OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过
```

**根因**：请求打到了 `tools/recv_server.py`（极简接收端，`do_GET` 对所有路径回同一句 alive），
它是 **`http.server.BaseHTTPRequestHandler`**；而带 `/ota.json` 逻辑的是
`rest_mock_server/server.py`（Flask）。

**一眼分辨谁在应答**（响应头里）：

| 头 | 应答者 |
|---|---|
| `Server: BaseHTTP/0.6 Python/3.x` | `recv_server.py`（没有清单） |
| `Server: Werkzeug/x.y Python/3.x` | `rest_mock_server/server.py`（有清单） |

**处理**：把设备要用的清单地址指到 8081（已改，见 §4.1）；核对：

```powershell
netstat -ano | Select-String ':(8080|8081)\s+.*LISTENING'
(Invoke-WebRequest 'http://127.0.0.1:8081/ota.json' -UseBasicParsing).Content
```

**判据**：返回体里有 `"version"` 键。

---

### 5.3 `request.host_url` 回环污染 ⇒ 设备去连它自己的 `127.0.0.1`

**现象**（很隐蔽）：清单里版本号明明更高、设备也**下载阶段卡住直到超时**，
或者你把清单贴进浏览器看是好的、设备却下不动。

**根因**：`server.py` 用 `request.host_url` 拼下载地址 —— 它是**照抄请求方用的主机名**：

- 设备自己 `GET http://192.168.0.101:8081/ota.json` → `url` 正确
- **但只要有人用 `localhost` / `127.0.0.1` 打开过一次**（浏览器、curl、`run_all.bat` 自检）
  → `url` 就变成 `http://127.0.0.1:8081/firmware/xxx.bin`，**设备会去连它自己的 127.0.0.1**
  → 必然超时。而 `run_all.bat` 的自检恰好就是 `curl http://localhost:%PORT%/ota.json`。

**处理**（已修）：`/ota.json` 里若解析出的主机是 `127.0.0.1` / `localhost` / `::1` / `0.0.0.0`，
就用 `_lan_ip()`（UDP socket 连 `8.8.8.8` 取 `getsockname()`，不发包、不做 DNS）换成真实局域网 IP，
日志会打：

```
[19:24:12] OTA   manifest 请求来自 localhost，改写 url 主机为 192.168.0.101（否则设备会连自己的 127.0.0.1）
```

**判据**：无论用 `localhost` 还是局域网 IP 请求，`url` 里的主机**都必须是局域网 IP**：

```powershell
(Invoke-WebRequest 'http://localhost:8081/ota.json' -UseBasicParsing).Content
(Invoke-WebRequest 'http://192.168.0.101:8081/ota.json' -UseBasicParsing).Content
# 两者 url 字段应当一模一样
```

---

### 5.4 Windows 允许**重复绑定同一端口** ⇒ "谁应答"不确定

**现象**：同一套配置，有时拿到 `{"server":"alive"}`，有时拿到真清单；过一阵又变了。
`netstat` 里**同一个端口出现两个不同 PID，都是 `LISTENING`**。

**根因**：`netstat` 实测（本次）：

```
TCP 0.0.0.0:8080  LISTENING  8236   ← tools/recv_server.py
TCP 0.0.0.0:8080  LISTENING  22956  ← ★ 改端口之前启动的旧 rest_mock_server 残留
TCP 0.0.0.0:8081  LISTENING  5396   ← 当前 rest_mock_server
```

两个进程都设了 `SO_REUSEADDR`（Windows 语义比 Linux 宽松），于是**都能绑上**，
连接落到谁手里取决于实现细节 —— 这就是"飘忽现象"的来源。**光看 `netstat` 有一行 LISTENING 是不够的，
要看是不是有多行、PID 是否是你要的那个。**

**处理**：杀掉残留的那个（按 PID，而不是按端口名）：

```powershell
# 1) 看是哪个进程，并确认命令行指向哪个脚本
netstat -ano | Select-String ':(8080|8081)\s+.*LISTENING'
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -like 'python*' } |
  ForEach-Object { $_.ProcessId.ToString() + ' | ' + $_.CommandLine }
# 2) 按 PID 杀（比 taskkill /IM python.exe 安全，不会误杀 IDF 的 python）
taskkill /F /PID <pid>
```

**判据**：每个端口**只有一行** LISTENING。

---

### 5.5 "只升不降"是刻意的 —— 想复现升级只能用**手动** `/api/ota` 降级

**现象/诉求**：设备已经是最新 0.1.2 了，想再测一次自动升级，但服务器版本不高于本机 ⇒ 永远不变。

**根因**：上电检查里 `清单版本 <= 本机` 一律跳过（防服务器上放旧固件把设备降级）。
这是**设计约束**，不是 bug。

**处理（本次用的手法，已验证）**：用**手动** `/api/ota`（无版本比较）把设备降到低版本，
然后**什么都不做**，看下一轮上电自己升回来。实测时间线（这就是一条完整的端到端判据）：

```
[ 21s] fw=0.1.0  part=ota_1   ← 手动推 v010 的 bin，降级成功、重启
[ 36s] fw=0.1.2  part=ota_0   ← ★ 上电自动检查发现 0.1.2 更高，自己下载、自己升级
```

服务端侧的铁证（三条都来自设备 `192.168.0.104`）：

```
19:32:55  GET /firmware/v010/esp32s3_snapshot_kit.bin   ← 手动推的降级
19:33:20  GET /ota.json                                  ← ★ 设备上电自己来查
19:33:20  GET /firmware/esp32s3_snapshot_kit.bin         ← ★ 自己下载 0.1.2
19:33:40  GET /ota.json                                  ← 升完再上电，版本相等 → 不再下载
```

**做法**（0.1.0 的 bin 放子目录，不影响 `/ota.json` 只挑顶层最新这一点）：

```powershell
# 1) 把本地 0.1.0 的 build 产物放进子目录
$fw  = '<本工程>\rest_mock_server\firmware'
Copy-Item '<本工程>\build\esp32s3_snapshot_kit.bin' "$fw\v010\esp32s3_snapshot_kit.bin" -Force
(Get-FileHash "$fw\v010\esp32s3_snapshot_kit.bin" -Algorithm MD5).Hash.ToLower()   # 取 md5

# 2) 手动推给设备（admin_key 为空时不需要鉴权头）
Invoke-WebRequest -Method POST -UseBasicParsing `
  'http://<设备IP>/api/ota?url=http://<pc-ip>:8081/firmware/v010/esp32s3_snapshot_kit.bin&md5=<md5>'

# 3) 轮询看版本/分区自己变回去
1..20 | ForEach-Object { Start-Sleep 3
  ((Invoke-WebRequest 'http://<设备IP>/api/status' -UseBasicParsing).Content | ConvertFrom-Json) |
    Select-Object fw_version, running_partition }
```

**注意**：`/api/ota` 会把这次 URL 存进设备的 NVS `ota_url`（不参与上电检查，无害）。

---

### 5.6 「烧了新固件却毫无变化」= 用的是 `idf.py app-flash`（otadata 没被重置）

**先记住一个事实**（`build/flash_args` 里明明白白列着）：**`idf.py flash` 会写 5 个位置**，
其中包含 otadata：

```
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0      bootloader/bootloader.bin
0x20000  esp32s3_snapshot_kit.bin
0x8000   partition_table/partition-table.bin
0xf000   ota_data_initial.bin          ← ★ otadata 被重置为初始值
```

`ota_data_initial.bin` 里 ota_0 / ota_1 两个条目都不是"有效待启动"状态，
**bootloader 于是回落到 `factory`**。这正好解释了真机现象：

```
I (110) boot: Defaulting to factory image
[BOOT ] ESP32-S3 Snapshot Kit v0.1.0        ← 设备此前明明跑的是 ota_0 / 0.1.2
```

**现象**：`idf.py` 烧录成功，但重启后 `fw_version` / `running_partition` 一点没变，
新固件"像没烧进去"。

**根因**：你用的是 **`idf.py app-flash`**（只写 app）而不是 `idf.py flash`（全量）。
`app-flash` 只把镜像写到 `factory`(0x20000)，**完全不碰 otadata**；
otadata 仍指着 `ota_0` ⇒ bootloader 继续从 `ota_0` 启动，你刚烧进 `factory` 的新固件只是躺在那儿。

**处理**（按目的挑一个）：

| 目的 | 命令 | 说明 |
|---|---|---|
| 让刚烧进去的固件真正生效 | `idf.py -p COMx flash` | 全量烧，**自带 otadata 重置**，重启即回 `factory` |
| 只想清掉 OTA 的启动选择 | `idf.py -p COMx erase-otadata` | 单独擦 `0xf000/0x2000`，重启也回 `factory` |
| 根本不想碰串口 | 走 §5.5 的 OTA 路子 | 让设备自己升到新版本 |

**判据**：开机日志出现 `Defaulting to factory image`，且 `GET /api/status` 的
`fw_version` 是你刚编的号、`running_partition` 是 `factory`。

> 反之亦然：**升级过 OTA 的设备（`ota_0`/`ota_1`）在下一次全量 `idf.py flash` 之后会自己回到
> `factory`**。看到分区从 `ota_0` 变回 `factory` 不是 bug —— 是全量烧录把 otadata 重置了。

---

### 5.7 内嵌网页改完**必须重新 build**（页面不会自己更新）

**根因**：`main/CMakeLists.txt` 用 `target_add_binary_data(${COMPONENT_LIB} web/index.html TEXT)`
把 `index.html` 编成 `_binary_index_html_start/end` 符号，
`web_server.c` 的 `h_index` 直接把这坨 `.rodata` 发出去。**网页不在 flash 文件系统里，改文件不影响已烧固件。**

**处理**：改完 `index.html` → `idf.py build` → 烧录或 OTA 升级。
顺带一条：**页面里凡是周期性刷新的控件都要做"值不变就不写"缓存**
（`lv_label_set_text` 那套在 Web 侧对应 `textContent`；本工程 `refreshStatus()` 5s 一次，
已用 `pill()` / `rows()` 统一写入，改动时注意别引入每轮无条件重绘）。

**判据**：`http://<设备IP>/` 的页面里能看到新文案（例如英文版应出现 `Latest snapshot`）。

---

### 5.8 COM 口被 `idf.py monitor` 占着 ⇒ 无法烧录

**现象**：

```
使用"0"个参数调用"Open"时发生异常:"对端口"COM9"的访问被拒绝。"
```

**根因**：`idf.py -p COM9 flash monitor` 会**一直占着串口**（monitor 不退）。
本机实测还发现**同时存在多组** `idf.py` / `idf_monitor.py` / `esp_idf_monitor` 进程
（多开了几个窗口，只有其中一个真正持有端口，其余是空跑）。

**处理**：先看谁在用，再决定：

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*COM9*' } |
  ForEach-Object { $_.ProcessId.ToString() + ' | ' + $_.CommandLine }
```

- 想烧录：**在 monitor 窗口按 `Ctrl+]` 退出**（别直接关窗口，留僵尸进程）；
- 想绕过串口：用 §5.5 的 OTA 方式。

**判据**：`idf.py -p COM9 flash` 不再报"访问被拒绝"。

---

### 5.9 网页「Log / Events」面板会显示**固件打的中文日志**（英文化的边界）

**背景**：`index.html` 已整页英文化（见 §6 的核对命令，`CJK chars = 0`），
但页面上「Log」面板取的是 `GET /api/log`（固件日志环形表），「Events」取 `GET /api/events`。

**实测**（2026-09-27，`/api/log` 80 行）：**13 行仍含中文**，且集中在两个模板：

```
[Q   W] telegram skipped (token/chat_id 未配置) id=11
[PIPEW] queue_mode=buffer sd=FAIL (无卡/降级，照片驻留 PSRAM)
```

开机那几十行中文更多（`[SD E] mount fail ...`、`[CFG W] upload_mode=both 但 ...` 等）。

**处理（若要"整页无中文"）**：需要翻译**固件侧**日志文案，约 60 条、散在 13 个文件
（`upload_queue.c` / `snapshot_pipeline.c` / `config_store.c` / `ota_check.c` / `sd_hal.c` /
`trigger_fsm.c` / `wifi_hal.c` / `mqtt_hal.c` / `main.c` / `ota_hal.c` / `camera_hal.c` /
`buzzer_hal.c` / `logger.c`）。

⚠️ **代价**：本工程文档与测试清单大量**用中文日志关键字当判据**（例如
`[SD E] mount fail`、`出厂运动学参数迁移`）。改日志文案必须**同步改 README / docs/testing*.md /
docs/troubleshooting.md**，否则下次照旧文档核对会全部对不上。

**判据**：`(Invoke-WebRequest 'http://<设备IP>/api/log').Content` 里 `[\u4e00-\u9fff]` 匹配数为 0。

---

### 5.10 ★ 「拍到的总是上一帧」= 驱动队列里提前采好的旧帧（2026-09-27 修复）

**现象**（用户报）：PIR 触发或 Telegram `/snap` 之后，上传的照片是**上一次触发时**的画面
—— 人明明已经站在镜头前了，照片里却是几秒前空着的房间；`/api/last_snapshot` 同样。

**根因**（读 `managed_components/espressif__esp32-camera/driver/cam_hal.c` 的 `cam_task()`
确认过，不是猜的）：

- 驱动有 `frame_cnt`(= `fb_count` = **2**) 个 fb 缓冲 + 一个 `frame_buffer_queue`；
- **只要队列里还有空位，它每次 VSYNC 就自己采一帧 push 到队尾**，满了才停
  —— 这才是 `CAMERA_GRAB_WHEN_EMPTY` 的真实语义："**有空位就采**"，**不是**"要才采"
  （`camera_hal.c` 里原先写的"按需取帧"是错的，已一并更正）；
- `cam_take()` 用的是 `xQueueReceive` ⇒ **取队首 = 最旧**的那一张。

于是本工程"几秒才抓一帧"的用法必然踩中：
上一次抓拍归还帧 → 立刻空出一个槽 → 驱动马上采一帧塞进去 →
几秒后触发时 `esp_camera_fb_get()` 拿到的正是**几秒前**躺进去的那张。

**量化证据**（0.1.2 修复后 `camera_hal_grab()` 会把旧帧的年龄打出来）：

```
[CAM W] drop stale frame len=14102 age=13038ms (早于本次请求)   ← 队里躺了 13 秒
[CAM W] drop stale frame len=14102 age=3955ms
[CAM W] drop stale frame len=14102 age=3920ms                   ← 两张 ~3.9 秒前（相差 1 帧周期）
[CAM  ] grab ok len=11979 age=36ms drained=2                    ← 丢掉后拿到 36ms 的**新帧**
```

**修法**（不猜阈值）：`fb->timestamp` 是驱动在**帧起始 VSYNC** 时刻打的
`esp_timer_get_time()`，所以直接判

```
fb->timestamp >= 本次调用时刻   ⇒  这帧是"请求之后才开始曝光"的
```

不满足就丢弃再取，最多丢 `CAM_GRAB_MAX_DRAIN`(=4) 张。判据与场景 / 分辨率 / 曝光时长**无关**。

**代价**：`grab` 从 0~1ms 变成 ~40ms（多等一个帧周期）—— 这是"要拍触发那一刻"必须付的钱。
`capture took` 因此约 350ms（其中 300ms 还是补光）。

**顺带解开一个悬案**：§9.9 里"补光 300ms 与不补光的画面亮度只差 0.2%"，
当时归因为"AEC 补偿了补光" —— 真实原因是**那张帧在灯亮之前就已经曝光完成**。

---

## 6. 调试命令速查（可直接复制）

```powershell
# ── 服务：谁在监听、是不是我要的那个 ─────────────────────────────
netstat -ano | Select-String ':(8080|8081)\s+.*LISTENING'
Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*' } |
  ForEach-Object { $_.ProcessId.ToString() + ' | ' + $_.CommandLine }

# ── 清单是否正确（两种请求必须给同一个 url） ─────────────────────
(Invoke-WebRequest 'http://localhost:8081/ota.json'      -UseBasicParsing).Content
(Invoke-WebRequest 'http://192.168.0.101:8081/ota.json'  -UseBasicParsing).Content

# ── 设备当前状态（版本 / 分区 / 队列 / mqtt） ─────────────────────
$s = (Invoke-WebRequest 'http://<设备IP>/api/status' -UseBasicParsing).Content | ConvertFrom-Json
$s.fw_version; $s.running_partition; $s.uptime_ms; $s.system.mqtt

# ── 手动触发升级（admin_key 为空时不需鉴权头） ───────────────────
Invoke-WebRequest -Method POST -UseBasicParsing `
  'http://<设备IP>/api/ota?url=http://<pc-ip>:8081/firmware/esp32s3_snapshot_kit.bin'

# ── 镜像头版本 / md5（不改文件、不用串口） ───────────────────────
$b = [System.IO.File]::ReadAllBytes('<...>\build\esp32s3_snapshot_kit.bin')
[System.Text.Encoding]::ASCII.GetString($b,0x30,32).Split([char]0)[0]     # 版本（偏移必须是 0x30）
(Get-FileHash '<...>\build\esp32s3_snapshot_kit.bin' -Algorithm MD5).Hash.ToLower()

# ── 网页英文化核对（纯 ASCII 校验） ─────────────────────────────
$t = [System.IO.File]::ReadAllText('<...>\main\web\index.html', [System.Text.Encoding]::UTF8)
([regex]::Matches($t,'[\u4e00-\u9fff]')).Count          # 期望 0
```

```bat
:: ── 服务与固件发布 ───────────────────────────────────────────
cd <本工程>\rest_mock_server
start.bat                 :: 起 8081（打印本机 IP 与要填设备的参数）
run_all.bat -nobuild      :: 只重启服务，不构建
run_all.bat               :: 停服务 → 构建 → 重启服务 → 自检
python publish_firmware.py                        :: build/*.bin → firmware/（自动 MD5）
python publish_firmware.py ..\build\xxx.bin --name v1.2.3.bin   :: 指定产物/改名
stop.bat / status.bat
```

---

## 7. 本次全流程实测时间线（可当作"一次标准升级"的模板）

| 时刻 | 动作 | 结果 / 证据 |
|---|---|---|
| 19:22:21 | 设备上电（服务**未**启动） | `[OTA W] ... ESP_ERR_HTTP_CONNECT`（§5.1） |
| 19:23:29 | 拉起 8081 服务 | `REST mock server listening on :8081` |
| 19:24:12 | 自检 `/ota.json`（localhost + LAN IP） | 回环被改写，两条 url 一致（§5.3） |
| 19:29:36 | 手动 `POST /api/ota` 推 0.1.2 | 设备立刻下载 → 重启 → `0.1.2 / ota_0` |
| 19:32:55 | 手动 `POST /api/ota` 推 `v010/`（0.1.0） | 降级成功 → `0.1.0 / ota_1` |
| 19:33:20 | **不做任何操作** | 设备上电自动查清单 → 自动下载 0.1.2（§5.5） |
| 19:33:40 | 升完再上电 | 版本相等 → 不再下载（"只升不降不等"生效） |
| — | 页面核对 | `live-fw` 行显示 `Firmware: v0.1.2   Partition: ota_0` |

附带记录：本次 **SD 未插卡**，`[SD E] mount fail err=0x107 → degrade queue_mode=buffer`，
照片驻留 PSRAM 后照常 HTTP 上传成功（`[UPLO ] http ok status=200`）——
**"SD 挂了"不等于"上传坏了"**，判据看 `/api/status` 的 `sd.state` 与 `queue.mode`。

---

## 8. 当前状态与遗留

**设备侧（2026-09-27 收口）**：`cam-2f44`，固件 **`0.1.2`**，运行分区 **`ota_1`**，
IP **`192.168.137.174`**（PC 的移动热点网段），`upload_mode=both`，
`http_url`/`mqtt_uri`/`APP_OTA_CHECK_URL` **三者统一为 `192.168.137.1`**（§4.4），
`admin_key` 为空，`retake_on_bad=1`、`flash_before_ms=300`、`frame_size=10`(VGA)、`jpeg_quality=12`，
`psram_min` 稳定（无泄漏）。

> ★ **版本号已被重置过一次**（2026-09-27，按用户要求），号段使用经过：
> `0.1.0` = 交付基线（一度是源码版本）→ `0.1.1` = 服务器上用于验证"上电自动升级"的版本
> → **`0.1.2` = 修复"抓到的是上一帧缓存"**（§5.10）。
> 因此本文里出现的 "0.1.5 / 0.1.8 / 0.1.9 / 0.1.12 起" 都是**此前那一轮编号**，
> 功能描述仍有效、号段已重置。

**本次"基线 → 自动升级"实测（可直接当模板）**：

| 时刻 | 动作 | 证据 |
|---|---|---|
| 23:37 | 手动 `/api/ota` 推 0.1.0（**无版本比较**，所以能"降"） | 设备 → `0.1.0 / ota_0` |
| 23:38 | 0.1.0 上电检查 | `GET /ota.json → v0.1.0`，`up to date ... (相同)` |
| **23:40:37** | **重启后上电检查** | `GET /ota.json → v0.1.1` + `GET /firmware/*.bin 200`（**自己下载**） |
| 23:41 | 升完再上电 | 设备 → **`0.1.1 / ota_1`**，`GET /ota.json → v0.1.1` 相等不再动 |

**工程侧遗留（按优先级）**：

1. **固件业务日志仍是中文** → 网页「Log/Events」面板会露中文（§5.9，约 60 条 / 13 文件）。
2. **Telegram 命令下发是好的** —— 这条曾被**误判成"死代码"**（只做了文本搜索、没查调用点就下结论），
   **特此更正**：`main.c` 的 `tg_poll_task`（`TG_POLL_INTERVAL_MS=3000`）周期调用
   `upload_telegram_poll_commands()`，启动点在 `main.c` 的 `tg_poll_start()`
   （刻意放在 safe mode 早退**之前**：故障时正是最需要 `/status` 看一眼、`/reboot` 重来一次的时候）。
   0.1.1 真机证据：`[TG ] command '/snap' from chat=7960301969` +
   `[TG ] sendPhoto ... trig=telegram` ⇒ 命令真的走完了"收到 → 触发 FSM → 抓拍 → 回图"。
   **验证手法（不必等人发消息）**：PC 上 `netstat -ano | findstr :443` 应看到中继在监听
   （设备经它连 `api.telegram.org`）；设备日志里出现 `[TG ] command ...` 即链路通。
   **教训**：`upload_telegram_poll_commands()` 的调用点在 `main.c` 里，用文本搜索很容易漏 ——
   下"死代码"这类结论前要用"找引用"(LSP) 或直接读 `main.c` 的启动序列。
3. `rest_mock_server/firmware/v010/` 是 §5.5 的**降级测试资产**，留着可一键复现该测试；
   不需要就删目录（它是子目录，**不影响** `/ota.json` 只挑顶层最新 bin 的行为）。
4. 改完 `version.h` 后要**同步重编**：`idf.py reconfigure build`（只 `build` 可能仍用旧的
   `PROJECT_VER`，见 `run_all.bat` 的注释 —— 这是"编译日志一切正常、版本却没变"的经典坑）。
   ⚠️ 反过来也踩过一次：**构建失败时 `publish_firmware.py` 照旧会把 `build/` 里的旧 bin 发布上去**
   （它只读镜像头报版本，不比对 `version.h`），所以改版本后要确认发布输出里的 `version` 是新号。
5. 用 OTA 验证新版本时，记得 `publish_firmware.py` 把新 bin 覆盖到 `firmware/`；
   `/ota.json` 取的是 **mtime 最新**的那个。
6. ★ **`main/app_conf.h` 里的 Telegram token / chat_id 目前是本机调试默认值，交付前必须清空**
   —— 见 §9.3，这是一条**交付阻塞项**。

---

## 9. 配置优先级 与 Telegram 联调（含交付前必清项）

### 9.1 ★ 优先级铁律：`/config.json` 一旦存在，`app_conf.h` 就基本失效

这是**最容易被误解**的一条，代码在 `config_store.c`：

```341:378:main/config_store.c
static esp_err_t load_or_create(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) {
        LOGI(TAG, "no config.json -> write defaults");   /* 只有这时才用 app_conf.h */
        return save_locked();
    }
    ...
    esp_err_t err = parse_into(&s_cfg, root);            /* 用文件里的值覆盖 */
```

拆开说：

| 情况 | 结果 |
|---|---|
| 设备**从没有过** `/config.json`（新芯片 / 刚恢复出厂） | 用 `app_conf.h` 的宏建初值，并**立刻落盘**成 `/config.json` |
| `/config.json` 已存在 | 只用文件里的值；**文件里缺少的键**才回落到 `app_conf.h` 的宏 |
| 用网页或 `POST /api/config` 保存过任意一项 | `save_locked()` 会把**整份配置**写回文件，此后 `app_conf.h` 的 Telegram 默认值对**这台设备**再也不生效 |

⇒ **结论**：只要用**配网页**配过 Telegram，`app_conf.h` 里那两行默认值对这台设备就**失效了**；
它们只对"从没配过"的新机器（或恢复出厂后的机器）起作用。

⇒ 所以：**改 `app_conf.h` 后光重新烧录是没用的**，必须再做一次**恢复出厂**
（`config_store_factory_reset()` 会先 `set_defaults()`，再从 `app_conf.h` 重建文件）。

### 9.2 Telegram 联调步骤（0.1.3 起）

**前置**：8081 服务在跑、`/ota.json` 已指向 `0.1.3`、设备当前是 `0.1.2`。

| 步 | 动作 | 判据 |
|---|---|---|
| 1 | 让设备升到 `0.1.3`：复位（走 §5.1 的上电自动检查）／手动 `POST /api/ota`／串口 `idf.py -p COM9 flash` | `/api/status` 的 `fw_version = 0.1.3` |
| 2 | 让 token/chat_id 生效，**二选一**：<br>· **快**：`POST /api/config` 写 token + chat_id（不丢 Wi-Fi）<br>· **验证交付流程**：恢复出厂（`app_conf.h` 的值被重建，但 Wi-Fi 会丢、需重新配网） | 开机**不再**出现 `[CFG W] upload_mode=both 但 telegram token/chat_id 不完整 -> Telegram 跳过` |
| 3 | 给 bot 发任意消息（如 `/status`） | 串口打 `[TG  ] command '/status' from chat=<你的chat_id>`，且 Telegram 收到回复 |

**可用命令**（`upload_telegram.c` 的 `tg_handle_command`）：

```
/snap    立即抓拍一张（走 trigger_fsm_fire，仍受冷却与 armed 约束）
/status  回一条摘要（版本 / 传感器 / SD / IP / RSSI / armed / 队列 / heap）
/arm     布防        /disarm  撤防        /reboot  重启
其它文本 → 回一句 "commands: /snap /status /arm /disarm /reboot"
```

★ 非授权 chat 发来的命令会被**直接丢弃**并打
`[TG  ] drop command from unauthorized chat=<id>` —— 只有 `telegram_chat_id` 那一个会话能控制设备。

**命令下发为什么以前完全不起作用**：`upload_telegram_poll_commands()` 虽然完整实现，
却在全工程**没有任何调用点**（死代码）。0.1.3 起由 `main.c` 的 `tg_poll_task` 每 3s 调用一次，
所以**必须升到 0.1.3 之后，命令才可能生效**。

> 附加坑：读 `getUpdates` 做调试时**不要带 `offset` 参数** —— 带了就等于把这批消息
> "确认消费"掉，设备之后再也收不到它们（本页 §6 里的自检命令都没有带 offset）。

### 9.3 ★ 交付前必清清单（硬性阻塞项）

`main/app_conf.h` 是工程里**唯一允许硬编码密钥**的地方（为了"一客户一份、改一处就烧"），
所以交付前必须逐项清空 —— 否则客户设备会把照片/遥测发到**你的**账号，
token / 口令也会被编进客户固件（`strings` 就能扒出来）。

```powershell
# 交付前自查：期望下面每一项都输出空串 ""
Select-String main\app_conf.h -Pattern 'APP_TELEGRAM_TOKEN|APP_TELEGRAM_CHAT_ID|APP_MQTT_PASS|APP_HTTP_HEADER_VALUE'
```

| 宏 | 当前值 | 交付时 |
|---|---|---|
| `APP_TELEGRAM_TOKEN` | `8800291160:AAE...`（本机调试用 bot @libulubulu_bot） | **清空 `""`** |
| `APP_TELEGRAM_CHAT_ID` | `7960301969`（本机私人会话） | **清空 `""`** |
| `APP_MQTT_PASS` | `snap-7Kq2mXv9Lp4R` | 清空或换客户自己的 |
| `APP_HTTP_HEADER_VALUE` | 空 | 保持空 / 换客户自己的 |

清空后重新 `idf.py reconfigure build` 出交付固件（**固件里不含任何密钥**）；
客户的 bot / 口令由他自己在配网页或 `POST /api/config` 填进去。

### 9.4 ★ 国内网络下：设备**直连不上 Telegram**（实测结论）

**现象**：token/chat_id 都配好了，设备日志仍反复打

```
[TG  W] retry 1/3 after 1000ms err=perform_0x7002
[TG  W] retry 2/3 after 2000ms err=perform_0x7002
```

**`0x7002` = `ESP_ERR_HTTP_CONNECT`** —— **TCP 层就没连上**，不是证书/时间问题
（证书问题会报 TLS 相关错误；本机 `[SYS ] sntp ok` 已证明时间正确）。

**排除法（每条都有日志证据）**：

| 能力 | 结果 | 结论 |
|---|---|---|
| 局域网 HTTP 上传（8080） | `[UPLO ] http ok status=200` | 设备联网正常 |
| MQTT（1883，局域网） | `[MQTT ] connected` | 同上 |
| 上电 OTA 检查（8081，局域网） | `up to date: server=0.1.3` | 同上 |
| SNTP（`pool.ntp.org`，公网 UDP） | `[SYS ] sntp ok` | DNS + 公网 UDP 正常 |
| **Telegram（`api.telegram.org:443`）** | `0x7002` ×N | **只有这一条不通** |

**为什么 PC 上 `getMe`/`getUpdates` 却正常**：PC 装了代理客户端（本机实测
`ProxyEnable=1` / `ProxyServer=127.0.0.1:7892`，并且多了一块 `v2cloud` TUN 网卡 `198.18.0.1`），
**PC 的流量走了代理**；ESP32 是**直连**。两者是完全不同的两条路。

**结论**：想让设备收发 Telegram，必须让**设备自己那条路**能到 `api.telegram.org`。

| 方案 | 说明 | 评价 |
|---|---|---|
| 换网络 | 手机热点 + 手机上开全局代理（设备连该热点） | 最快验证；交付现场要单独确认 |
| 旁路由 / 路由器级透明代理 | 设备的网关指向能做代理的设备 | 现场可行，需额外硬件/配置 |
| **PC 侧桥接**（推荐） | 设备只跟局域网通信（已经很稳），由 PC 上的脚本把设备事件转发到 Telegram、把 Telegram 命令转成设备 HTTP 调用 | **不动固件**；本工程本来就依赖 PC 跑 8080/8081 服务 |

**⚠️ 别指望"PC 开 TUN 模式"能帮到设备**：TUN 只改 PC 自己的路由，ESP32 的流量根本不经过 PC。
而且开了 TUN 之后，PC 访问局域网设备**反而会被劫持** —— 实测：

```
Find-NetRoute -RemoteIPAddress 192.168.0.104
  → InterfaceAlias = v2cloud (198.18.0.1), NextHop 198.18.0.2, RouteMetric 0
arp -a 192.168.0.104  → No ARP Entries Found        # 真实网卡根本没发过 ARP
curl http://192.168.0.104/api/status      → 无输出（TCP 握手"成功"但拿不到数据）
```

症状极具迷惑性：`Test-NetConnection 192.168.0.104 -Port 80` 会报 **True**（TUN 客户端假握手），
但所有 HTTP 请求都超时。**这在代理客户端里放行私有网段直连即可恢复**
（Clash：`IP-CIDR,192.168.0.0/16,DIRECT` 或打开 Bypass LAN；v2rayN：路由规则加私网直连）。

**固件侧没有代理支持**：IDF 5.5.1 的 `components/esp_http_client` 里
**搜不到任何 `proxy` 字段**（整个组件目录 grep `proxy` 为 0 命中），
所以"给 Telegram 填个代理地址"这条路在当前 IDF 下不存在，要自己实现 HTTP CONNECT 隧道。

**判据（一眼分流）**：

| 日志 | 含义 | 查什么 |
|---|---|---|
| `0x7002` + 局域网通道全绿 | **出口不通** | 网络/代理，别再折腾 token |
| `401 Unauthorized` / `chat not found` | token / chat_id 有问题 | 重新 `/revoke` 或核对 chat_id |
| 完全没有任何 `[TG ]` 行 | 任务/配置没生效 | token+chat_id 是否都非空、是否 ≥0.1.3 |

### 9.5 ★ 通用"出口连通性探针"：拿 `/api/ota` 当设备的 GET 工具

`/api/ota` 接受**任意** `http(s)` URL，所以可以让设备去连任意地址来探测出口，
而且**不会真刷机**（不是合法镜像时 `esp_ota_end` 会失败，只记日志、不重启）：

```powershell
# 让设备去连任意 URL，再在 /api/log 里看结果
Invoke-WebRequest -Method POST -UseBasicParsing `
  'http://<设备IP>/api/ota?url=https://api.telegram.org/'
```

实测输出（**与 Telegram 模块完全独立**的代码路径）：

```
[OTA  ] start url=https://api.telegram.org/ md5=(skip)
[OTA E] download failed: download fail http=0 err=0x7002
[OTA E] ota failed: download fail http=0 err=0x7002 (不重启，继续运行 v0.1.3)
```

`http=0`（没拿到任何 HTTP 响应）+ `0x7002` = TCP 层就没连上。

**价值**：两条互不相干的路径给出同一结论 ⇒ 可以立刻排除"Telegram 模块写错了 / token 不对"，
把范围锁死在出口网络。侧信道提示：同一个 `/api/log` 里 `[SYS ] sntp ok` 说明
**公网 UDP 是通的**，只有到 Telegram 的 TCP 被挡 —— 这是"目标被单独阻断"的典型特征
（而不是"设备整体没网"）。

### 9.6 ⚠️ 把设备接到"PC 的移动热点"并不等于让设备走代理

想借 PC 的代理让设备出墙时，第一反应是"设备连 PC 的移动热点"。**实测这条路不通**：

| 观察 | 说明 |
|---|---|
| 设备 ip=`192.168.137.125`（`本地连接* 2`，PC 的移动热点网段） | 连上了 |
| `arp -a` 里设备 MAC 出现在 192.168.137.125 | 二层通了 |
| `[SYS ] sntp ok` | 转发到公网**是通的**（UDP） |
| `[OTA E] ... err=0x7002` + `[TG W] ... 0x7002` | 到 Telegram 的 TCP **仍然被挡** |

⇒ ICS 只做了 NAT 转发，**没有把设备流量送进 TUN 代理**（ICS 的"共享源"大概率是物理网卡；
且很多代理客户端的 TUN 不处理**来自其他主机**的转发包）。
**必须**在"移动热点 → 共享我的以下网络连接"里选到 TUN，或改用别的方案（§9.4 表）。

**另一个副作用**：设备在 PC 热点上时，**够不到 PC 上的局域网服务**（实测 `mqtt=disconnected`，
照片上传同样失败）—— 因为 `以太网 4` 的网络类别是 **Public**，Windows 防火墙默认**拦截入站**，
而"从热点子网进来到 PC"算入站。要让它同时能用局域网服务，得给 `192.168.137.0/24`
放行 TCP 1883/8080/8081，或把网络类别改成 Private。

> **后续实测更正**：ICS 的共享源**本来就是 `v2cloud`**（用 `HNetCfg.HNetShare` 查出来的，
> `SharingConnectionType=0`=PUBLIC），而且 `本地连接* 2` 与 `v2cloud` 的 IPv4 转发
> **本来就是 Enabled**。所以"共享源选错 / 没开转发"都不是原因 —— 就是**代理客户端不处理
> 转发包**。另外把设备的上报地址改成 `192.168.137.1` + 放行防火墙之后，
> `[MQTT ] connected`、`[OTA ] up to date`、`[UPLO ] http ok` **全部恢复正常**。

### 9.7 ★ 最终可用方案：PC 侧透明中继（`rest_mock_server/telegram_relay.py`）

§9.4 / §9.6 的方案（换网络 / 旁路由 / 指望 ICS 把设备流量带进 TUN）在本机都试过，
**只有这一条通**：

```
设备 --(把 api.telegram.org 解析到 192.168.137.1)--> PC 上的中继 (0.0.0.0:443)
                                                        |
                                                        +-- CONNECT --> PC 代理 127.0.0.1:7892 --> Telegram
```

**为什么它能成**：中继是 PC 上的**普通本机进程**，它自己发起的连接天然走 VPN；
设备只要把域名解析到 PC 就连得过来。中继只搬字节，**TLS 是"设备 ↔ Telegram"端到端**的，
所以这等于真正验证了设备自身的 Telegram 实现，而不是绕过它。

三步（详见 `rest_mock_server/telegram_relay.py` 与 `rest_mock_server/relay_setup.ps1`）：

```powershell
# 1) 起中继
python rest_mock_server\telegram_relay.py          # 监听 0.0.0.0:443，上游走 127.0.0.1:7892

# 2) 管理员：域名指向 PC + 放行 443
powershell -ExecutionPolicy Bypass -File rest_mock_server\relay_setup.ps1

# 3) 自测（模拟设备路径：连 PC 的 443，SNI 用真域名）
curl.exe --resolve "api.telegram.org:443:192.168.137.1" "https://api.telegram.org/bot<token>/getMe"
```

**成功判据**（中继日志每 3s 一组，正好对应 `tg_poll_task` 的轮询节奏）：

```
[21:48:04] [relay] 客户端接入 192.168.137.174:55990
[21:48:04] [relay] 上游就绪（经代理 127.0.0.1:7892）
```

⚠️ 两个注意：
- 中继是**临时进程**，重启 PC 就没了；hosts 那行却会一直生效 ——
  若中继没起，**PC 自己访问 `api.telegram.org` 也会失败**。
  撤销：`relay_setup.ps1 -Remove`。
- ★ **新发现的坑**：`server.py` 的 `_lan_ip()` 取的是"默认路由对应的接口地址"，
  而 TUN 模式下默认路由是 TUN ⇒ 用 `localhost` 打开 `/ota.json` 时改写出的 url 会变成
  `http://198.18.0.1:8081/...`（TUN 的地址）。设备用 `192.168.0.101` 访问时**不受影响**
  （非回环不触发改写），但**用 localhost 自检时不要相信那个 url**。

### 9.8 ★ 修掉的两个真实固件 bug（Telegram"一直不通"的真正内因）

网络打通后立刻暴露出两个**一直存在、却从未有机会表现**的 bug：

**bug 1：`esp_http_client_perform()` 之后又调 `read_response()`，响应体永远是空的**

```c
/* 错：perform() 已经把 body 读掉并丢弃了 */
esp_http_client_perform(cli);
esp_http_client_read_response(cli, resp, sizeof(resp) - 1);      /* -> 0 字节 */
if (status == 200 && strstr(resp, "\"ok\":true")) { ... }        /* -> 永远不成立 */
```

根因（IDF 源码 `components/esp_http_client/esp_http_client.c`，`esp_http_client_perform()` 末尾）：

```c
while (client->response->is_chunked && !client->is_chunk_complete) { get_data(); }
while (client->response->data_process < client->response->content_length) { get_data(); }
```

**正确写法**（已改，`main/upload_telegram.c` 两处）：

```c
esp_http_client_open(cli, body_len);          /* 只发请求头 */
esp_http_client_write(cli, body, body_len);   /* body 必须自己 write */
esp_http_client_fetch_headers(cli);
int status = esp_http_client_get_status_code(cli);
int rd = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
esp_http_client_close(cli);
```

影响面：`tg_post_simple()`（`getUpdates` / `sendMessage` / `getMe`）与 `sendPhoto()` **两处都中**。
**症状极具迷惑性 —— 完全静默**：`SET_ERR` 只写错误串不打印、调用方又忽略返回值，
串口一条日志都没有；而中继日志却显示请求**成功到达并拿到 200**。

**bug 2：`upload_telegram_poll_commands()` 全工程没有任何调用点**（见 §9.2），
已由 `main.c` 的 `tg_poll_task` 补上（3s 一次）。

**修复后真机验证（0.1.4）**：

```
[TG   ] command '/start'   from chat=7960301969
[TG   ] command '/disarm'  from chat=7960301969
[TRIG ] armed=0                                    ← 命令真的改动了设备状态
[TG   ] command '/snap'    from chat=7960301969
[TRIG ] src=telegram rejected: disarmed            ← /snap 被"已撤防"规则正确拒绝
...
[TG   ] sendPhoto chat=7960301969 len=9932 caption="ESP32-S3 Snapshot Kit v0.1.4 | ..."
[TG   ] sendPhoto ok retry=0                       ← ★ 照片真的发出去了
[Q    ] upload ok id=1 ok=1 fail=0                 ← HTTP + MQTT 同时正常
```

### 9.9 「照片发灰 / 像上传坏了」= 那一瞬间镜头前没有可拍内容

**现象**：Telegram 收到一张**整片均匀灰白**（带淡淡斜向渐变）的照片，
第一反应必然是"上传坏了 / 编码坏了"。**实测这条路是错的** —— 需要按下面顺序排除。

**判定手法（两条，都很便宜）**：

1. **连拍 3 帧比 md5**：三帧 md5 各不相同 ⇒ 相机在输出**实时变化**的画面，
   排除"死帧 / 缓冲区没刷新"。
2. **看 JPEG 长度**：空帧、整片纯色、糊掉的帧都会**明显偏小**
   （实测灰图 ≈ 7KB，正常实景 14~47KB 视细节而定）。

**补光是不是元凶？实测不是**（用 System.Drawing 采样算画面均值亮度，同一静止场景 A/B）：

| `flash_before_ms` | 抓拍长度 | 画面均值亮度 |
|---|---|---|
| 0（不补光） | 46736 B | **140.2** |
| 300（补光 300ms） | 46766 B | **139.9** |

两帧 md5 不同（确实重拍了），但**亮度只差 0.2%**。

**后续确认（用户目视）**：把 `flash_before_ms` 临时设成 `3000` 后点「立即拍照」，
板子上的白光 LED **确实亮了 3 秒** ⇒ `PIN_FLASH = 21` 正确、灯是好用的。

所以那 0.2% 的亮度差意味着：**传感器 AEC 自动补偿了补光带来的额外照度**（把曝光压回去），
**因此不会过曝**。

⇒ **结论定案：灰图来自物理遮挡**（镜头被贴住 / 前面是一面没有纹理的表面），与补光无关。
⇒ 保留 `flash_before_ms` 默认 300ms 是合理的（灯是好用的，用户目视确认过它确实会亮）。

> ⚠️ **上面那张"亮度只差 0.2%"的表，归因已被推翻（2026-09-27）**：真实原因是
> **那张帧在补光点亮之前就已经曝光完成** —— 驱动队列里的旧帧，见 §5.10，不是"AEC 补偿"。
>
> 另外要记住：**"画面平均亮度"这个测法对补光本来就不敏感** —— AEC 会把平均亮度拉回
> 目标值，所以补光开/关都测出 ~144（0.1.2 上重测：`flash=0 → 144`、`flash=300 → 144`）。
> 要评估补光的真实作用，该看**运动模糊 / 噪声**（补光让它能用更短曝光），而不是均值亮度。

> 补光的实现位置：`snapshot_pipeline.c` 抓拍前点亮、**抓拍后立刻关**
> （注释写明"否则 OV5640 长曝光会过曝 / 灯板发热"），逻辑本身没问题。

**代码对策（0.1.8 起）：判为坏帧就立刻补拍一张**

> 0.1.5~0.1.7 用过"一次连抓 3 帧、留 JPEG 最大的那张"。**已废弃**：每多抓一帧就是
> +1 个传感器帧周期（~20ms），而绝大多数帧都是好的 —— 为少数坏帧让**每一次**抓拍
> 都付这个代价不划算。现在是"先拍一张；判为坏帧才补拍一张"。

`snapshot_pipeline.c` 的 grab 段：**单拍 → 判坏 → 立刻补拍一张 → 取两帧里更大的那张**。

正常情况（零额外开销）：

```
[PIPE ] grab start flash=300ms retake=on ref=0        ← ref=最近一张合格帧的体积，开机后第一张为 0
[PIPE ] grab ok len=11929 fb=0x3c140eb8
[PIPE ] capture took=309ms flash=300ms grab=1ms retake=0    ← grab 只 1ms，没有补拍
```

坏帧情况（**真机实测**，2026-09-27 0.1.9：运行时把分辨率从 VGA 切到 QVGA 触发的；
那时 `SNAP_BAD_LEN_PCT` 还是 60，所以触发线是 `60% × 56292 = 33775`）：

```
[PIPE ] grab start flash=300ms retake=on ref=56292
[PIPEW] bad frame len=8868 < 33775 -> retake (1/1)     ← 判为坏帧，立刻补拍
[PIPE ] retake len=2997 (first=8868) -> keep 8868      ← 补拍那张反而更小 -> 留原来那张
[PIPE ] grab ok len=8868 fb=0x3c140eb8
[PIPE ] capture took=319ms flash=300ms grab=2ms retake=1   ← 补拍只多花 ~1ms
```

坏帧判据（**只用 JPEG 体积**这一个免费信号，实现见 `snapshot_pipeline.c` 顶部）：

| 项 | 值 | 说明 |
|---|---|---|
| 基准 `ref` | 最近一张**合格帧**的体积 | 实测 VGA+q12 正常实景 12~18KB（场景越有细节越大） |
| 触发线 | `ref × 35%`（**严格线**，2026-09-27 定）；**只有还没有基准时**才用 4096B 兜底 | 含义 = "只有细节几乎全没了的帧才补拍"。★ 兜底值**不能**在有基准时也参与取大 —— QVGA+高压缩的正常帧只有 ~3KB，那样会每次抓拍都误判补拍 |
| 基准更新（4 条，全是踩坑后加的） | ① 坏帧按 7/8 缓慢下移；② 合格帧**单次最多上跳 1.5 倍**；③ 分辨率/画质一变就**整块作废**；④ 作废时顺手**冲掉驱动里残留的 2 张旧模式帧**（`fb_count=2`）并丢弃重标定后的第一个样本 | 防：连续坏帧把基准拖没 / 一帧"大而坏"顶高基准 / 换模式后十几次白补拍 / **旧模式的帧被当成新基准** |
| 开关 | `retake_on_bad`（0/1，**默认 1**） | 配网页 Configuration 卡片可改；0 = 每次只拍一张 |

**★ 它抓不到的一类坏帧：`大而坏`。** 实测切到 QVGA 后的**第一帧**是 56292B
（正常 QVGA 帧 ~4KB 的 14 倍，传感器刚切模式的瞬态产物）—— 体积判据只能判"太小"，
对这种又大又坏的帧无能为力（真要抓它得解码看内容，约 50ms，不值）。
对策是"换模式后丢弃第一个样本"，而不是去判别它。

**★ 换模式为什么要整套重置（实测踩了三次才理清）**：

1. `ref` 被顶到 5 万后靠 7/8 缓慢回落要**十几次**抓拍（PIR 低频场景下就是几小时），
   期间每次都在白补一张 ⇒ **发现模式变了就立刻作废，不要等衰减**；
2. 但光"作废"还不够：`fb_count=2`，驱动里还留着**两张旧模式的帧** ——
   实测切到 QVGA 后第一、二次抓到的仍是 VGA 尺寸的老帧（17233），被当成新基准，
   于是真正的 QVGA 帧（6066）反被判成坏帧、白补一张 ⇒ 作废时必须**冲掉这 2 张残留帧**；
3. 冲掉之后第一帧就是新模式的了，再配合"丢弃重标定后的第一个样本"，
   第二次抓拍基准就正确（实测 6053），全程 `retake=0`。

修完后的实测（0.1.12：VGA → QVGA → VGA 各连抓数次）：

```
[PIPE ] 模式变化 framesize/q 10/12 -> 6/12：体积基准 17261 作废，重新标定
[PIPE ] 体积基准未建立：丢弃本帧样本 len=6082          ← 冲掉旧帧后，首帧已是 QVGA 尺寸
[PIPE ] capture took=380ms flash=300ms grab=66ms retake=0   ← 66ms 里含"冲 2 张旧帧"
[PIPE ] grab start flash=300ms retake=on ref=6053
[PIPE ] grab ok len=6044
[PIPE ] capture took=310ms flash=300ms grab=0ms retake=0    ← ★ 不再白补
```

★ **为什么用"相对基准"而不是固定字节阈值**：0.1.5 设计时**否决**过"空帧守卫"，
理由正是"固定阈值区分不了`画面糊`和`场景本身简单`（对着一面纯白墙，正常帧本来也只有几 KB）"。
改成与**最近一张合格帧**比较后，这条否决理由不再成立：持续简单的场景会自己重新定基线。

**★ 标定过程（2026-09-27 真机两次遮挡实测，最终定 75）**：

| 遮挡方式（补光开着） | 帧体积 | 占正常帧 | 35% 线 | 60% 线 | **75% 线（现用）** |
|---|---|---|---|---|---|
| 手指贴住镜头 | **11575 B** | **67%**（正常 17.3KB） | ✗ 漏 | ✗ 漏 | ✅ 触发 |
| 手掌 / 纸挡住 | **7436 B** | **19%~44%**（正常 17~38KB） | ✅ | ✅ | ✅ 触发 |

真机日志（`SNAP_BAD_LEN_PCT=75`）：

```
[PIPE ] grab start flash=300ms retake=on ref=35319
[PIPEW] bad frame len=7436 < 26489 -> retake (1/1)      ← 判定
[PIPE ] retake len=7428 (first=7436) -> keep 7436       ← 补拍那张更小 -> 留原来那张
[PIPE ] capture took=318ms flash=300ms grab=2ms retake=1  ← 代价 +2ms
```

⇒ **"整片糊"在体积上最多只掉到 67%**（此时 JPEG 由噪声与渐变主导，不是细节），
而正常场景的帧间波动也能到 73%（17233 → 12591，有人走过）—— **两者重叠**。
所以 **75 是唯一能覆盖全部实测遮挡、又不必"每张都补"的取值**；
代价是正常波动也会偶发补拍：实测 `grab` 由 0~1ms 变 2ms，且最终取更大的那张，不亏。

> 早先文档里写的"灰图 ≈7KB、占正常 15%~50%"是**另一套配置**（编译期 q14）下的数字，
> 对当前 VGA + q12 + 补光的组合**不成立**，已按上表更正。

★ **它救不了运动模糊**（必读，免得误解这个功能）：同一次触发的第二张，
场景 / 曝光时间 / 运动速度都没变，拍出来一样糊。补拍真正能救的是**瞬态坏帧**：
AE/AWB 还没收敛、驱动首帧、整片纯色 / 全黑、偶发撕裂帧。
要"夜里走动的人不糊"，该动的是**补光与曝光**，不是补拍。

★ **帧所有权（改这段前必读）**：全程**最多只持有 2 帧**（原帧 + 补拍帧），
落败方立刻 `camera_hal_return()` 归还；`fb_count=2` 的驱动缓冲池因此不会被占死。
最终保留的那帧仍走原来的单帧流程，**唯一归还点依旧是 `done:` 标签处**，不要新增归还点。

> ★ 与本判据强相关的一个问题（**0.1.9 已修**）：
> 配置里的 `frame_size` / `jpeg_quality` 曾经**根本没应用到摄像头** ——
> `camera_hal_set_param()`（`main/camera_hal.c`）在整个工程里**没有任何调用点**，
> `camera_hal_init()` 只用编译期的 `CONFIG_SNAP_DEFAULT_FRAMESIZE` 与
> `default_quality_for(fs)`。⇒ 网页上改分辨率 / 画质只会被写进 `/config.json`，
> 抓拍结果纹丝不动，而界面显示"已保存" —— 与 §9.2 里"Telegram 命令**曾**因没有调用点而完全失效"
> 属于同一类问题：**功能实现完整，但没人调用**（那处已在 0.1.3 修好，这次是我又看走眼说成"仍是死代码"）。
>
> **0.1.9 的修法**：
> - `camera_hal_init()` 直接读运行期配置 —— 必须在 **init 处**定，因为 framebuffer 是
>   按 init 时的 framesize 分配的，事后改大缓冲区不够（画面截断/花屏且不报错）；
> - `POST /api/config` 保存后再调 `camera_hal_apply_runtime()`，让改动立即生效；
>   该函数**只放行"改小/同尺寸"**，改大会打警告并提示重启（等 init 用新尺寸重新分配）；
> - `/api/status` 的 `camera.jpeg_quality` 从"配置请求值"改成"**实际生效值**"
>   （新增 `camera_hal_current_quality()`），这样界面再也不会替传感器撒谎。
>
> ⇒ 本节所有体积数字都是 **0.1.8 的编译期配置（VGA + q14）** 下的实测值。
> 0.1.9 起质量改听配置的 `jpeg_quality`（默认 12，比 q14 更清晰、体积更大），
> 体积基准整体上移；判据是**相对量**所以不用重标定，但绝对值别再照抄本节。
>
> ★ 顺带纠正一处编号错误（配网页原来写 `10 = SVGA`，**错**）：
> esp32-camera 2.1.7 的枚举从 `0 = 96x96` 起算，中间还有 `128x128` / `240x240` /
> `320x320` 三个极易漏掉的档位，正确对应是
> **`10 = VGA(640x480)`（默认）、`11 = SVGA(800x600)`、`12 = XGA(1024x768)`**。
> `SENSOR_MAX_FS`：OV2640 = `UXGA(15)`、OV3660 = `QXGA(19)`、OV5640 = `QSXGA(23)`
> —— `camera_hal.c` 里原来的 `/* 13 */` `/* 17 */` 两个注释也是错的，已一并改掉。

**没有采用的方案**（记下来避免重复讨论）：

| 方案 | 为什么没做 |
|---|---|
| 连拍 3 帧取最优（0.1.5~0.1.7 用过） | 每次抓拍都付 ~2 个帧周期（30~60ms），只有少数帧是坏的 ⇒ 性价比低，已由"判坏补拍"取代 |
| 解码后算清晰度（梯度方差） | 工程里现成的 esp_jpeg/TJpgDec 全尺寸解码 ≈ 50ms，**比补拍本身还贵**；阈值还随场景漂移 |
| 默认关闭补光 | 已确认灯是亮的、且 AEC 会把它补偿掉 ⇒ 补光在暗环境下确实有用，**保留默认 300ms** |
| PIR 触发后延迟 N ms 再拍（等目标站稳） | 对"人走过来"的运动模糊最有效，但会漏掉前半段动作；本工程未采用 |

### 9.10 ★ 抓拍耗时预算与"最快档"（0.1.8 实测）

**先纠正一个极易误判的点**：`/api/snapshot?force=1` **不绕冷却**（已在 `docs/api.md` 里更正）。
连拍验收时两次触发必须间隔 ≥ `min_trigger_interval_ms`（默认 3000ms），
否则第二次拿到 `409 cooldown`，看起来像"拍照失败"。

**设备侧打点**（0.1.7 起，`snapshot_pipeline.c` 在 `done:` 处打一行）：

```
[PIPE ] grab start flash=300ms retake=on ref=15994      ← ref = 最近一张合格帧的体积
[PIPE ] grab ok len=15932 fb=0x3c140eb8
[PIPE ] capture took=309ms flash=300ms grab=1ms retake=0
```

- `capture took` = 进入 pipeline → 入队完成（**不含**网络与上传）
- `flash`  = 补光 `vTaskDelay` 的固定开销
- `grab`   = 取帧总耗时（含补拍那次抓取）
- `retake` = 本次是否补拍（0/1）

**实测（2026-09-27 真机 0.1.2，OV3660 / VGA / q12 / flash=300）**：

| 配置 | capture took | flash | grab | retake |
|---|---|---|---|---|
| 单拍、帧合格（**0.1.2 起：丢旧帧 + 真抓一帧**） | **350 ms** | 300 | **39~43 ms** | 0 |
| `flash_before_ms=0`（最快） | **~55 ms** | 0 | ~40 ms | 0 |
| 补拍一次（判为坏帧时） | +~40 ms | — | ×2 | 1 |
| 单拍（**0.1.1 及以前**，grab 只从队列取旧帧） | 309 ms | 300 | **0~1 ms** | 0 |

**三条结论**：

1. ★ **0.1.2 之前那个"grab=1ms"是假快** —— 它只是把驱动队列里**几秒前**就采好的帧取出来
   （§5.10 有 `age=13038ms` 的实测证据）。现在必须真等一帧曝光完成，所以
   `grab ≈ 40ms`（≈ 一个帧周期）；传感器自己出 JPEG 的搬运本身只要 ~1.6ms。
2. **补拍不再"免费"** —— 边际成本是一次真抓帧（≈ 一个帧周期），但仍远小于 300ms 补光，
   所以坏帧判据依旧可以放心用。
3. ★ **300ms 补光仍是绝对大头**（350 − 40 ≈ 310ms 全在 `vTaskDelay(flash_before_ms)`）。
   想要"最快"就把 `flash_before_ms` 设 0。

**取舍**：

| 想快 | 想稳 |
|---|---|
| `flash_before_ms=0` —— 省 300ms；代价是暗环境画面偏黑 | `flash_before_ms=300`（默认；AEC 会补偿，不会过曝，见 §9.9） |
| `retake_on_bad=0` —— 每次只拍一张，永不补拍 | `retake_on_bad=1`（默认；边际成本 1ms，白拿的兜底） |

**无泄漏判据**（连续 3 次抓拍实测）：`snapshot.psram_min` 保持 **8124160** 不变（不单调下降），
且 `[CAM] grab` / `return` 配对 —— 补拍路径里落败的那帧当次就归还，不会累积到下一次。

> 两条与本主题相关、别误判的既有行为：
> - **Telegram 同一个 chat 5 秒内只发一张**：快速连拍时日志出现
>   `[TG  W] throttled (chat=.., 2917ms since last, limit=5000ms)`，
>   该事件 Telegram 记 fail，但 HTTP 通道成功后整体仍是 `upload ok`。
> - 上传（HTTP 8080 + Telegram）与抓拍是**异步**的 —— `capture took` 只描述抓拍本身，
>   照片"到手机"还要几百 ms ~ 几秒的网络时间。
