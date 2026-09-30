# ESP32-S3 Snapshot Kit（Freenove ESP32-S3-WROOM / N16R8）

基于 **ESP-IDF v5.5.1** 的定时/触发式抓拍上传套件。同一套代码通过 **编译期 Kconfig**
切换摄像头型号（OV2640 / OV3660 / OV5640）与 SD 三态（NONE / OPTIONAL / REQUIRED），
产出 6 个可独立烧录的固件。

> **禁止 Arduino**。全工程只用 ESP-IDF 原生 API。
> 详细资料见 [`docs/`](docs/)：接线 / 传感器 / SD 三态 / API / 测试 / 排错。
>
> **测试请从 [`docs/testing_full.md`](docs/testing_full.md) 开始** —
> 它是以当前代码为准的权威清单；`docs/testing.md` 是首版设计态文档，
> 其中 TAG 名 / SD 退避 / AP 行为 / 上传失败语义已有变更（勘误表见前者 §12）。
>
> **升级 / 端口 / 内嵌网页 / 串口出问题，先看 [`docs/debug_notes.md`](docs/debug_notes.md)** —
> 2026-09-27 真机实测的完整链路与逐条踩坑记录（8080 与 8081 的分工、
> `request.host_url` 回环污染、上电只检查一次、COM 口被 monitor 占用等最易误判的坑）。

---

## 1. 硬件

| 项目 | 规格 |
|---|---|
| 模组 | Freenove ESP32-S3-WROOM（ESP32-S3-N16R8） |
| Flash | 16 MB（本工程实际用 8 MB 偏移表，见 `partitions.csv`） |
| PSRAM | 8 MB **Octal (OPI)**，80 MHz |
| 摄像头 | OV2640（默认）/ OV3660 / OV5640，24Pin FPC |
| microSD | 板载卡槽，**SDMMC 1-bit**（CLK=39 / CMD=38 / DAT0=40） |
| 板载灯 | WS2812 @ GPIO48（RMT 驱动）+ 绿色 LED @ GPIO2 |

**引脚唯一来源是 [`main/pins_config.h`](main/pins_config.h)**，其它任何文件都不得硬编码 GPIO。
完整对照表见 [`docs/wiring.md`](docs/wiring.md)。

---

## 2. 编译

### 2.1 环境

```powershell
& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null
cd d:/ESP32-IDF/esp-idf-v5.5.1/examples/get-started/esp32S3_CAM
```

### 2.2 单次编译

```powershell
idf.py set-target esp32s3
idf.py build
```

产物：`build/esp32s3_snapshot_kit.bin`（**注意工程名是 `esp32s3_snapshot_kit`**，
不是目录名 `esp32S3_CAM`，写脚本判断产物时别搞错）。

烧录 + 监视：

```powershell
idf.py -p COM6 flash monitor      # Ctrl+] 退出
```

### 2.3 全配置矩阵（6 组）

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_matrix.ps1
```

逐组覆盖 `tools/cfg_overrides/sdkconfig.<组名>.defaults`，每组都
`fullclean` + 删除 `sdkconfig` 从零生成，产出 `build_log_<组名>.txt`：

| 组名 | 传感器 | SD 模式 | 其它 |
|---|---|---|---|
| `ov2640_optional` | OV2640 | OPTIONAL | 默认配置 |
| `ov3660_optional` | OV3660 | OPTIONAL | |
| `ov5640_optional` | OV5640 | OPTIONAL | |
| `ov2640_sd_none` | OV2640 | **NONE** | 纯 PSRAM 队列 |
| `ov2640_sd_required` | OV2640 | **REQUIRED** | 无卡即 safe mode |
| `full_features` | OV5640 | OPTIONAL | + MQTT + MJPEG stream |

期望输出末行：

```
RESULT: 6 / 6 configurations built OK
```

实测（2026-09-26，IDF v5.5.1-dirty）：

```
ov2640_optional     1302448 bytes
ov3660_optional     1304944 bytes
ov5640_optional     1305136 bytes
ov2640_sd_none      1238016 bytes
ov2640_sd_required  1302432 bytes
full_features       1333632 bytes
```

---

## 3. 上电自动升级（版本检查）

设备**每次上电、Wi-Fi 连上之后**会去服务端问一次版本号，比自己的大就自动升级并重启。

```
上电 → Wi-Fi STA 连上 → GET <APP_OTA_CHECK_URL>（几百字节 JSON）
     → 清单版本 > 本机版本 ? 下载 .bin → MD5 校验 → 写 OTA 槽 → 重启
                              : 什么都不做（日志一行 up to date）
```

**与手动升级（`POST /api/ota`）是两条并存的路径**：手动那条推 URL、立即执行；
这条是上电时自己判断。两者共用 `ota_hal` 的下载/校验/写分区逻辑，不会并发。

### 3.1 开关与地址

改 `main/app_conf.h` 一处即可：

```c
#define APP_OTA_CHECK_URL   "http://192.168.0.101:8080/ota.json"
```

**留空 = 关闭上电自动升级**（手动那条仍可用）。改完重新编译烧录。

### 3.2 服务端怎么准备

用工程自带的 `rest_mock_server`，**它同时提供清单和固件，不需要另开静态服务器**：

```powershell
cd examples/get-started/esp32S3_CAM/rest_mock_server
python server.py                 # 监听 8080，提供 /ota.json 与 /firmware/*.bin

# 另开一个窗口：把刚编译出来的固件发布过去
python publish_firmware.py       # 从 ..\build\ 复制 .bin 并算 MD5
```

发布脚本会打印**从镜像头读出来的版本号**，拿它和 `main/version.h` 对一眼：

```
published esp32s3_snapshot_kit.bin  (1343872 bytes, 1.28 MB)
md5       6321a4c8...
version   0.1.1                    ← 必须等于 version.h 里的 SNAP_FW_VERSION
```

### 3.3 清单格式

`GET /ota.json` 返回（`md5` 可省略；`url` 由服务端按请求的 host 自动生成）：

```json
{"version": "0.1.2", "url": "http://192.168.0.101:8080/firmware/x.bin", "md5": "..."}
```

**版本号是服务端从 `.bin` 的 `esp_app_desc_t` 里读出来的**，不是你手填的 ——
这样清单和它指向的镜像**不可能对不上**。

### 3.4 升级新版本的完整操作

> **★ 版本号只有一处要改：`main/version.h` 的 `SNAP_FW_VERSION`。**
> 该文件顶部有完整的规则说明，`_NUM` 只是陪衬（无代码使用，CMake 会提示不同步）。

1. 改 `main/version.h` 的 `SNAP_FW_VERSION`（比如 `0.1.1` → `0.1.2`）；
2. `idf.py build`；
3. `python publish_firmware.py`（覆盖 `firmware/` 里的旧 .bin）；
4. **重启设备**（或等它下次上电）→ 自动升级。

**为什么"改了版本号却升不了"** —— 按可能性排序：

| 现象（串口） | 原因 | 处理 |
|---|---|---|
| `check version: running=0.1.1` 后**没有** `new firmware`，而是 `up to date` | 服务器上那份 .bin 是**旧的**（改完版本号没重新 publish），或 publish 的是没重新 build 的旧 .bin | 重跑第 2、3 步；`publish_firmware.py` 会打印读出来的版本，对一眼 |
| `manifest -> HTTP 404` | `rest_mock_server` 里 `firmware/` 目录没有 .bin，或服务器是**改代码前启的旧进程**（Python 不热加载） | 停掉旧进程重启 `server.py` |
| `GET … failed: ESP_ERR_HTTP_CONNECT` | 服务器没开 / IP 变了（`APP_OTA_CHECK_URL` 写死在 `app_conf.h`） | 确认 `curl http://<ip>:8080/ota.json` 能出 JSON |
| 完全没有 `check version` 这行 | 设备还没连上 Wi-Fi（AP 配网中），检查被跳过 | 先让它连上 STA |
| 每次都升、**反复重启** | 版本号写成了非数字（如 git describe 产物）被解析成大数 | 见 §故障速查；`version.h` 必须纯数字 |

> 排序依据：实际踩到最多的是第 1 条 —— **「改的是设备版本号」不是升级手段**。
> 要让设备升上去，是让**服务器上的镜像**比设备新，而不是把设备改成更大的号。

### 3.5 判据（怎么确认它真的在工作）

| 想要的结果 | 串口应出现 | 其它现象 |
|---|---|---|
| 已是最新 | `[OTA] check version: running=0.1.1 url=…`<br>`[OTA] up to date: server=0.1.1 running=0.1.1 (相同)` | 一切照常，不重启 |
| 有新版本 | `[OTA] new firmware: 0.1.1 -> 0.1.2`<br>`[OTA] auto-update ok, rebooting...` | 约 10~60s 后重启，重启后日志显示新版本 |
| 服务器没开 | `[OTA] GET … failed: ESP_ERR_HTTP_CONNECT（跳过自动升级）` | **启动/业务完全不受影响** |
| 不想让它跑 | `[OTA] auto-update disabled (APP_OTA_CHECK_URL 为空)` | —— |

服务端侧同时会打印一行，可用来确认设备真的来过：
`[HH:MM:SS] OTA   ota.json -> v0.1.2 esp32s3_snapshot_kit.bin`

### 3.6 三个必须知道的约束

1. **只升不降**。清单版本 ≤ 本机版本一律跳过，所以服务端放着旧固件不会导致回滚 ——
   反过来说，**刷错版本后无法用这个机制退回**，只能手工烧。
2. **检查任务不阻塞启动**。它跑在独立任务（8KB 栈）里，会先等 STA 连上（最多 60s）。
   AP 配网模式下（还没连 Wi-Fi）会直接跳过，日志
   `[OTA] STA 未连上（AP 配网中？）-> 跳过上电版本检查`。
3. **检查排在 safe mode 早退之前**（`main.c` 第 9b 步）。这是刻意的：设备功能不完整
   （SD/摄像头故障）时**正是最需要靠 OTA 修复的场景**，放在早退之后会让坏设备永远停在旧固件。

---

## 4. 首次上电

1. **不插 SD 卡**也必须能跑：默认 `SD_MODE_OPTIONAL`，无卡自动降级到 PSRAM 内存队列。
2. 手机/电脑连热点 **`ESP32S3-Setup`**，密码 **`12345678`**；
3. 浏览器打开 **`http://192.168.4.1/`**（或 `http://esp32s3cam.local/`）进配置页；
4. 填 Wi-Fi SSID/密码 → 保存 → 设备重启后进 STA 模式；
5. 串口看到 `[SYS] ... armed=1` / 绿灯慢闪 = `[READY]`。

---

## 5. 日志规范

一行一条，格式固定（`tools/parse_log.py` 依赖这个格式，改格式要同步改脚本）：

```
[<TAG>] <message>
```

日志由 `esp_log` 输出，TAG 用 `esp_log` 自带的 5 字符**左对齐方括号**填充：

```
I (12345) [BOOT] esp32s3-snapshot-kit v0.1.1 sensor=OV2640(0x2642) sd_mode=OPTIONAL
W (12401) [SD  ] mount fail err=0x105 -> degrade queue_mode=buffer retry=30s
E (18902) [CAM ] init failed err=0x105
```

TAG 取值（**以 [`main/logger.h`](main/logger.h) 的 `LOG_T_*` 宏为准**）：

| TAG | 宏 | 含义 |
|---|---|---|
| `BOOT` | `LOG_T_BOOT` | 启动横幅、芯片信息、就绪 |
| `CFG` | `LOG_T_CFG` | 配置读写 |
| `WIFI` | `LOG_T_WIFI` | STA/AP/重连 |
| `CAM` | `LOG_T_CAM` | 摄像头 init / grab / return |
| `SD` | `LOG_T_SD` | SD 状态机 |
| `TRIG` | `LOG_T_TRIG` | 触发状态机 |
| `PIPE` | `LOG_T_PIPE` | 拍照管道 / 分流 |
| `Q` | `LOG_T_Q` | 上传队列 |
| `UPLOAD` | `LOG_T_UPLOAD` | HTTP 上传 |
| `TG` | `LOG_T_TG` | Telegram |
| `MQTT` | `LOG_T_MQTT` | MQTT |
| `WEB` | `LOG_T_WEB` | web server |
| `OTA` | `LOG_T_OTA` | OTA |
| `SYS` | `LOG_T_SYS` | 时间同步 / 重启 / 杂项 |
| `LED` | `LOG_T_LED` | LED / 按键 |

> 注意：**没有** `UPLD` / `HUMAN` 这两个 TAG。
> HTTP 与 Telegram 分别用 `UPLOAD` 和 `TG`；
> 人形检测 stub 复用 `PIPE`（因为它在拍照管道里跑）。
> `tools/parse_log.py` 的已知 TAG 集合与上表同步。

---

## 6. 目录结构

```
esp32S3_CAM/
├── CMakeLists.txt              project(esp32s3_snapshot_kit VERSION …)
│                               ★ 从 main/version.h 解析版本传给 PROJECT_VER，
│                                 确保镜像头版本与本机版本号同源（见 §7 无限刷机）
├── partitions.csv              factory + 2×OTA + littlefs（含 otadata，见 §7.1）
├── sdkconfig.defaults          默认配置（OV2640 + SD OPTIONAL）
├── main/
│   ├── pins_config.h           ★ 引脚唯一来源
│   ├── version.h               ★ 版本号唯一来源（SNAP_FW_VERSION）
│   ├── app_conf.h              ★ 交付配置唯一入口（Wi-Fi/MQTT/OTA 检查地址）
│   ├── Kconfig.projbuild       传感器选择 / SD 三态 / 功能开关
│   ├── main.c                  app_main，只做启动编排
│   ├── camera_hal.*            三传感器统一接口
│   ├── sd_hal.*                SD 三态状态机
│   ├── upload_queue.*          上传队列（path/buffer 双模式）
│   ├── upload_http.*           multipart 上传
│   ├── upload_telegram.*       Telegram sendPhoto + 命令轮询
│   ├── mqtt_hal.*              MQTT（默认关闭）
│   ├── web_server.*            HTTP API + mDNS
│   ├── web/index.html          内嵌单页控制台
│   ├── ota_hal.*               手动升级：https + MD5 预校验 + 回滚
│   ├── ota_check.*             ★ 上电自动升级：取清单 → 比版本 → 调 ota_hal
│   ├── app_version.*           版本字符串解析/比较（纯 C，无 IDF 依赖）
│   ├── led_hal.*               状态指示灯语义
│   ├── buzzer_hal.*            蜂鸣器（FATAL 报警短鸣）
│   ├── trigger_fsm.*           PIR/按键/远程触发状态机
│   ├── snapshot_pipeline.*     grab → 分流 → 归还
│   ├── config_store.*          littlefs /config.json
│   ├── wifi_hal.*              STA 60s → AP
│   ├── time_sync.*             SNTP 非阻塞
│   ├── button_hal.*            板载 BOOT0(GPIO0)：短按人脸识别 / 长按 5s 恢复出厂
│   ├── human_detect.cpp        ★ 真模型人脸检测（必须是 .cpp，见 docs/debug_notes.md §0）
│   ├── human_stub.*            仅日志占位（与 human_detect 编译期二选一）
│   └── logger.*                TAG 与日志宏
├── rest_mock_server/
│   ├── server.py               REST 上报接收 + ★ /ota.json + /firmware/ 托管
│   └── publish_firmware.py     把 build/*.bin 发布到 firmware/（自动算 MD5）
├── test/                       ★ PC 上的回归测试，不参与固件构建
│   ├── version_guard_test.py   版本格式守卫全用例（防无限刷机）
│   └── version_guard_test.c    同上，给编译器看（无 x86 gcc 时只做语法检查）
├── tools/
│   ├── build.ps1               单次编译
│   ├── build_matrix.ps1        6 组矩阵
│   ├── cfg_overrides/          各组 sdkconfig 覆盖
│   ├── test_api.py             API 自测（无需 pytest）
│   └── parse_log.py            串口日志解析/统计
├── docs/                       wiring / sensors / sd_modes / api / testing / troubleshooting / debug_notes
└── build_log_<组名>.txt        6 组编译日志
```

---

## 7. 关键设计约束（改代码前先读）

1. **传感器是编译期切换**，不是运行时探测。
   `CONFIG_CAM_OV2640/OV3660/OV5640` 决定代码分支，`CONFIG_<型号>_SUPPORT`
   决定固件里链进哪个驱动 —— 两者必须同步改，见 [`docs/sensors.md`](docs/sensors.md)。
2. **framebuffer 有且只有一个归还点**。
   `esp_camera_fb_get()` 后每一条路径都要 `esp_camera_fb_return()`；
   队列接管 `copy` 所有权后原指针必须置 `NULL`，防止 double free。
3. **冷却判断必须单线程**（串行 trigger 任务）。
   否则"连拍 50 次"验收会出现并发拍照。
4. **所有网络操作都有超时 + 有限重试**，禁止 `while(!connected)` 死等。
5. **禁止硬编码密钥**。admin_key / Telegram token 都来自 `/config.json`，
   日志里只打前 6 位。
6. **不做人脸识别/活体检测**。"人形检测"只有 `human_stub.c` 的日志占位。
7. **版本号只有一个来源：`main/version.h` 的 `SNAP_FW_VERSION`。**
   顶层 `CMakeLists.txt` 会解析它并设给 `PROJECT_VER`，从而写进镜像头的
   `esp_app_desc_t.version`。**不要删掉那段 CMake** —— 删掉后 IDF 会退回用
   `git describe`（形如 `676dc691-dirty`），而上电 OTA 比较会把它当成 `676.x`
   从而**每次上电都升级，无限刷机**（详见 §8）。版本号也必须是纯数字加点，
   写成 `0.1.1-rc1` 会被 `ota_check` 的格式守卫拒绝。

---

## 8. 排错

常见现象 → 根因对照表见 [`docs/troubleshooting.md`](docs/troubleshooting.md)，
其中几条最容易踩：

- 摄像头 `init failed 0x105` → `CAM_PIN_D0..D7` 顺序错，或 SCCB 上拉缺失；
- `esp32s3_snapshot_kit.bin` 找不到 → 脚本写成了目录名 `esp32S3_CAM.bin`；
- 换型号后 5MP 配不上 → 忘了同步 `CONFIG_<型号>_SUPPORT`，固件里没这个驱动。

### 8.1 ★「设备不停地重启」（开了自动升级之后）

**现象**：串口反复出现 `[OTA] new firmware: 0.1.1 -> 676.0.0` →
`auto-update ok, rebooting...` → 重启后又来一遍。设备永远在刷同一个固件。

**根因**：镜像头里的版本号不是 `0.1.1` 而是 **git describe** 的产物
（形如 `676dc691-dirty`）。`app_version_parse()` 只认数字，把它读成 `676.0.0`，
恒大于 `0.1.1` ⇒ 每次上电都判定"有新版本"。

**成因**：IDF 在 `PROJECT_VER` 未定义时按 `version.txt` → `PROJECT_VERSION` →
**`git describe`** 取值。本工程原先没有前两者，于是落到第三条。

**核对方法**（一条命令看出真相）：

```powershell
cd examples/get-started/esp32S3_CAM/rest_mock_server
python -c "import sys; sys.path.insert(0,'.'); import server; print(repr(server.read_image_version(r'..\build\esp32s3_snapshot_kit.bin')))"
# 期望输出 '0.1.1'；若输出 '676dc691-dirty' 之类，就是本问题
```

**修复**：顶层 `CMakeLists.txt` 必须保留从 `main/version.h` 解析版本并
`project(... VERSION "${SNAP_VERSION}")` 的那段（见 §7 第 7 条）。
有了它，两个数字永远同源。改完记得 **`idf.py fullclean`** 再编译 ——
`PROJECT_VER` 缓存在 CMake cache 里，只 rebuild 不重新配置不会生效。

**第二层防护**：`ota_check.c` 的格式守卫会直接拒掉 `676dc691-dirty` 这类
非 `数字[.数字]` 的清单版本，日志：

```
[OTA] 清单 version="676dc691-dirty" 不是 数字[.数字] 形式 -> 拒绝比较（若放行会导致每次上电都升级，无限刷机）
```

守卫的回归测试在 `test/version_guard_test.py`（PC 上直接 `python` 跑，
期望 `ALL PASS`）。

### 8.2 ★「升级流程全部成功，但版本号就是不变」= 分区表缺 otadata

**现象**（日志按顺序全部是"成功"的样子，极具误导性）：

```
[OTA  ] check version: running=0.1.0 url=http://192.168.0.101:8080/ota.json
[OTA  ] new firmware: 0.1.0 -> 0.1.1 (md5 已提供)      ← 判定有新版本，对
[OTA  ] downloaded 1346112 bytes (1.28 MB)             ← 下载完成，对
[OTA  ] md5 ok f9bc0b244ea4fea7109a02a01ba683ac       ← 校验通过，对
I (23585) esp_image: segment 0: paddr=00310020 ...     ← 镜像确实写进 ota_0 了
I (23876) esp_ota_ops: not found otadata              ← ★ 唯一的错误线索
[OTA E] ota failed: set_boot_partition 0x105 (不重启，继续运行 v0.1.0)
```

重启后 `[BOOT ] … v0.1.0`，**版本号纹丝不动**。

**根因**：分区表里有 `ota_0` / `ota_1`，但**没有 `otadata` 分区**。
ESP-IDF 靠 `otadata` 记录"下次启动该用哪个 OTA 分区"，缺了它
`esp_ota_set_boot_partition()` 直接返回 `0x105` (`ESP_ERR_NOT_FOUND`)。

注意 `0x105` 在这条路径上的含义**不是**"image 校验失败"（那是
`esp_ota_end` 的 `0x105`），而是"**找不到 otadata 分区**"。同一错误码两处含义，
容易误判成固件损坏。

**修复**（`partitions.csv`，`otadata` 必须在 `nvs` 之后）：

```csv
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,     ← 8KB = 2 个扇区，固定这个大小
phy_init, data, phy,     0x11000, 0x1000,
factory,  app,  factory, 0x20000, 0x300000,
ota_0,    app,  ota_0,   0x320000,0x300000,
ota_1,    app,  ota_1,   0x620000,0x300000,
littlefs, data, spiffs,  0x920000,0x6D0000,
```

三个必须注意的点：

1. **`otadata` 长度必须是 `0x2000`（2 个扇区）**，不能只给 `0x1000` —— IDF 需要
   两张扇区交替写入来做掉电保护。
2. **`otadata` 插进来后，后面所有分区的 offset 都要顺移**。插入 `0x2000` 后
   `phy_init` 从 `0xf000` 挪到 `0x11000`；app 分区必须 **64KB 对齐**，所以
   `factory` 从 `0x10000` 挪到 `0x20000`（`0x12000` 也 64KB 对齐，但用 `0x20000`
   留出余量），`ota_0/ota_1/littlefs` 依次 +0x10000。
3. **`littlefs` 必须缩小到 `0x6D0000`**，否则 `0x920000 + 0x6F0000 = 0x1010000`
   **超出 16MB flash**，`idf.py build` 直接报分区表越界。
   分配顺序：`0x920000 + 0x6D0000 = 0xFF0000`，恰好落在 16MB 内（余 64KB）。

**为什么必须 `fullclean`**：分区表偏移变了，旧 `build/` 里的
`partition-table.bin` 与 `bootloader` 都是按旧布局生成的，只 rebuild 不会重算。
改完分区表必须：

```powershell
idf.py fullclean
idf.py build
idf.py -p COM<x> flash monitor
```

**烧录后要确认一次 otadata 真的存在** —— 看启动日志的 partition table 段：

```
I (55) boot: ## Label            Usage          Type ST Offset   Length
I (61) boot:  0 nvs              WiFi data        01 02 00009000 00006000
I (68) boot:  1 otadata          OTA data         01 00 0000f000 00002000   ← 必须有这行
I (74) boot:  2 phy_init         RF data          01 01 00011000 00001000
```

> ⚠️ 改分区表会**清掉 NVS**（Wi-Fi 凭据）和 littlefs（`/config.json`）——
> 偏移变了等于换了地方，设备会像全新的一样进 AP 配网模式。
> 这是预期行为，重新配网即可。

**判据**：升级日志末尾变成

```
[OTA  ] write ok part=ota_0 -> reboot in 1s to apply v0.1.1
```

重启后 `[BOOT ] ESP32-S3 Snapshot Kit v0.1.1` —— 版本号终于变了。

**防呆**：`ota_hal.c` 对 `set_boot_partition` 的 `ESP_ERR_NOT_FOUND` 做了
专门识别，日志会直接点名"分区表缺少 otadata 分区"，不用再去猜 `0x105` 是什么意思。
# ESP32-CAM
