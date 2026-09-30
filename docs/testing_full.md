# 全功能测试清单（以**当前代码**为准）

> 本文件与 [`testing.md`](testing.md) 的关系：`testing.md` 是**设计态**首版清单，
> 其中有若干条已被后续代码改动推翻（见文末 §12 勘误）。
> **本文是权威版本**，每条的日志关键字都对着 `main/*.c` 核过。
>
> 串口：`idf.py -p COM6 monitor`，退出 `Ctrl+]`。
> 产物名是 `build/esp32s3_snapshot_kit.bin`（**不是** `esp32S3_CAM.bin`）。

---

## 0. 测试环境与总判据

| 项 | 值 |
|---|---|
| 板子 | Freenove ESP32-S3-WROOM (N16R8)，OV2640，**不插 SD 卡**跑主路径 |
| 默认 IP（STA） | 见串口 `[WIFI ] connected ip=192.168.x.x`，本文示例用 `192.168.0.104` |
| AP 配网 IP | `192.168.4.1`，SSID `ESP32S3-Setup`，pass `12345678` |
| 日志格式 | `I (t_ms) [TAG  ] message`，TAG **5 字符左对齐** |

**贯穿全程的 4 条全局判据**（每轮测试结束都看一遍）：

| # | 判据 | 期望 |
|---|---|---|
| G1 | 搜 `FB-OVF` | **0 条**（摄像头帧缓冲配置正确） |
| G2 | 搜 `err=none` | **0 条**（无通道不再误报失败） |
| G3 | `GET /api/status` 的 `snapshot.psram_min` | 反复拍照后**不单调下降** |
| G4 | `[CAM ]` 的 `grab` 行数 vs `return` 行数 | **配平**（差 0） |

---

## 1. 编译与烧录

| # | 操作 | 判据 |
|---|---|---|
| 1.1 | `idf.py build` | `Project build complete.`，无 `error` |
| 1.2 | `powershell -File tools/build_matrix.ps1` | 末行 `RESULT: 6 / 6 configurations built OK` |
| 1.3 | 校验传感器开关成对 | `Select-String sdkconfig -Pattern '^CONFIG_OV\w+_SUPPORT=y'` **只有一行**；`^CONFIG_CAM_OV\w+=y` **只有一行**；两者同型号 |
| 1.4 | `idf.py -p COM6 flash monitor` | 首行 `[BOOT] esp32s3-snapshot-kit vX.Y.Z sensor=OV2640(0x2642) sd_mode=OPTIONAL` |
| 1.5 | 紧接一行 | `[BOOT] idf=v5.5.1 chip=ESP32-S3 rev=? flash=16384KB psram=8192KB(OCT) reset=...` |
| 1.6 | 清空 NVS 后首启 | `[WIFI ] 未配置 wifi_ssid -> 直接进入 AP 配网` |

> `reset=POWERON` 是正常上电；`BROWNOUT` 是供电问题（换后置 USB 口），先别怀疑代码。

---

## 2. 启动与就绪（不插卡）

| # | 操作 | 判据 |
|---|---|---|
| 2.1 | 不插 SD 卡复位 | 不卡在挂载，继续往下走 |
| 2.2 | 看 SD 日志 | `[SD  ] mount start clk=39 cmd=38 d0=40 mode=1bit` → `mount fail err=0x... -> degrade queue_mode=buffer` |
| 2.3 | 看是否到就绪 | 出现 `[READY]` |
| 2.4 | 搜 `fatal` / `safe mode` | **都搜不到**（OPTIONAL 不插卡是合法路径） |
| 2.5 | 看板上灯 | WS2812 启动红闪 → 就绪后**绿灯慢闪** |

**★ 这是任务书硬性要求**：OPTIONAL 无卡必须能启动、配网、拍照、上传。

---

## 3. Wi-Fi 与 AP 配网

### 3.1 正常运行（已配网）—— ★ 本轮重点验证

| # | 操作 | 判据 |
|---|---|---|
| 3.1.1 | 已配好 Wi-Fi 时启动 | `[WIFI ] connected ip=192.168.x.x rssi=-NN` |
| 3.1.2 | 搜 `ap started` | **0 条**（已配网不再无条件开热点） |
| 3.1.3 | 手机搜热点 `ESP32S3-Setup` | **搜不到** |
| 3.1.4 | 串口搜 `+ softAP` | **搜不到**（只有 `wifi:mode : sta (...)`） |
| 3.1.5 | 搜 `ap channel adjust` | **搜不到**（AP 没起，无需调信道） |

### 3.2 配网路径（必须仍然可用，别改死）

| # | 操作 | 判据 |
|---|---|---|
| 3.2.1 | 清空 Wi-Fi 配置后复位 | `[WIFI ] 未配置 wifi_ssid -> 直接进入 AP 配网` |
| 3.2.2 | 紧跟 | `[WIFI ] ap started ssid=ESP32S3-Setup ip=192.168.4.1` + `请在浏览器打开 http://192.168.4.1 配置 Wi-Fi` |
| 3.2.3 | 手机连 `ESP32S3-Setup` / `12345678` | 连接成功，串口 `[WIFI ] ap client joined <MAC>` |
| 3.2.4 | 浏览器 `http://192.168.4.1/` | 出现控制台页面 |
| 3.2.5 | 填 Wi-Fi 保存 | `{"ok":true}`；串口凭据更新行 |
| 3.2.6 | **配网成功后约 3 s** | `[WIFI ] AP 配网完成 -> 关闭热点，仅保留 STA` |
| 3.2.7 | 再搜热点 | `ESP32S3-Setup` **消失**；STA 保持连接 |
| 3.2.8 | 重启 | 直接进 STA，不再开热点 |

### 3.3 mDNS

| # | 操作 | 判据 |
|---|---|---|
| 3.3.1 | 浏览器 `http://esp32s3cam.local/` | 与用 IP 访问同样出现页面 |

---

## 4. 拍照主链路

| # | 操作 | 判据 |
|---|---|---|
| 4.0 | `POST /api/arm` | `{"armed":true}`（**不布防后面会 409**） |
| 4.1 | `POST /api/snapshot` | `{"accepted":true,"force":false}` |
| 4.2 | 串口随后异步出现 | `[CAM ] grab ...` + `[PIPE]` 分流行 + `[Q   ]` 入队行 |
| 4.3 | 3 s 内连点 5 次 | 第 2 次起 `409 {"error":"cooldown"}` |
| 4.4 | `POST /api/snapshot?force=1` | `{"accepted":true,"force":true}`，绕冷却 |
| 4.5 | 撤防后 `?force=1` | 仍 `409 {"error":"disarmed"}` ← **force 绕不过布防** |
| 4.6 | `GET /api/last_snapshot` | 返回 JPEG，浏览器能直接看图 |
| 4.7 | `GET /api/photos` | 顶层是**数组** `[...]`，不是 `{"files":[...]}` |

**内存验收（关键）**：

```powershell
python tools/test_api.py --host 192.168.0.104 --burst 50
```

| # | 判据 | 期望 |
|---|---|---|
| 4.8 | `snapshot.psram_min` | 50 次连拍中**不单调下降** |
| 4.9 | `sys.free_heap` / `sys.free_psram` | 无单调下降趋势 |
| 4.10 | `grab` / `return` 配平 | 差 0（G4） |

---

## 5. SD 卡三态

### 5.1 OPTIONAL（默认）

| # | 操作 | 判据 |
|---|---|---|
| 5.1.1 | 插卡启动 | `[SD  ] mount ok total=NNNMB free=NNNMB cid=...`，`GET /api/sd` 的 `state=SD_OK` |
| 5.1.2 | 拍照 | `GET /api/photos` 有内容；路径 `snapshots/YYYY/MM/DD/` |
| 5.1.3 | 运行中拔卡再拍 | 连续 3 次写失败后 `[SD  ] write fail count>=3 state=FAIL -> degrade` |
| 5.1.4 | 拔卡后拍照 | 仍成功；`queue.mode` 变 `buffer`；`path=null` **不是错误** |
| 5.1.5 | `POST /api/sd/remount`（带 key） | `{"ok":true,"err":0,"state":"SD_OK"}` |
| 5.1.6 | 低空间清理 | 看 `[SD  ] low space ... -> cleanup` 后 `free=` **前后差值**；卡里全是未上传文件时**什么都不删是设计行为** |

### 5.2 无卡退避重试 —— ★ 本轮重点验证

| # | 操作 | 判据 |
|---|---|---|
| 5.2.1 | 不插卡，观察 10 分钟 | `[SD  ] retry mount start (attempt N)` **不超过 5 次**（退避 30s→60s→120s→…封顶 10min；原来是固定 30s = 20 次） |
| 5.2.2 | 看失败日志 | `remount fail` **静默**，只在第 1/10/20… 次打一条（原来每次都刷） |
| 5.2.3 | 10 分钟内插卡 | 立刻 `[SD  ] remount ok recovered state=OK` |
| 5.2.4 | 再看退避 | 计数**清零**（再拔卡从 30 s 重新起算） |

### 5.3 NONE

| # | 操作 | 判据 |
|---|---|---|
| 5.3.1 | 烧 `ov2640_sd_none` 固件 | `[BOOT] ... sd_mode=NONE` |
| 5.3.2 | 紧接 | `[SD  ] mode=NONE -> sd_hal disabled (all calls return NOT_SUPPORTED)` |
| 5.3.3 | `GET /api/sd` | `"mode":"none"` 且 `"state":"SD_DISABLED"` |
| 5.3.4 | 插卡也没用 | 仍 `SD_DISABLED`（编译期关死） |

### 5.4 REQUIRED

| # | 操作 | 判据 |
|---|---|---|
| 5.4.1 | **不插卡**启动 | `[SD  ] mode=REQUIRED and mount failed -> fatal` |
| 5.4.2 | LED | **红灯常亮** |
| 5.4.3 | web | 仍能进配置页（safe mode 保留 web） |
| 5.4.4 | `POST /api/snapshot` | `503 {"error":"safe mode: camera pipeline disabled"}` |
| 5.4.5 | 插卡重启 | 正常进 `[READY]` |

---

## 6. 上传（HTTP / Telegram / MQTT）

### 6.1 无通道（默认现网状态）—— ★ 本轮重点验证

| # | 操作 | 判据 |
|---|---|---|
| 6.1.1 | 不配 `http_url` / Telegram，触发拍照 | `[Q   ] no channel configured id=N -> 已保留(SD)/丢弃(内存) 本次事件未上送` |
| 6.1.2 | 搜 `err=none` | **0 条**（不再有假错误日志） |
| 6.1.3 | `GET /api/status` 的 `queue` | `fail_count=0` **不增长**；`ok_count=0` |
| 6.1.4 | 语义确认 | "没配出口" != "上传失败"，计数分离 |

### 6.2 HTTP 上传

| # | 操作 | 判据 |
|---|---|---|
| 6.2.1 | `POST /api/config` 填 `http_url`（带 key）→ 拍照 | `[UPLOAD] ... status=200` |
| 6.2.2 | 故意配错 URL | 出现 `retry` 行，**不阻塞后续拍照** |
| 6.2.3 | `GET /api/status` 的 `queue` | `fail_count` 递增；`last_error` 有内容 |

> ★ TAG 是 **`UPLOAD`**，不是 `UPLD`（`UPLD` 在全工程不存在，grep 不到别以为没日志）。

### 6.3 Telegram

| # | 操作 | 判据 |
|---|---|---|
| 6.3.1 | 配 `telegram_token` + `telegram_chat_id`，`upload_mode=telegram` | `[TG  ] sendPhoto ok` |
| 6.3.2 | token 打印 | 日志里**只出现前 6 位**，如 `123456***` |

### 6.4 MQTT（默认关）

| # | 操作 | 判据 |
|---|---|---|
| 6.4.1 | `CONFIG_SNAP_ENABLE_MQTT=n`（默认） | `GET /api/status` 的 `sys.mqtt = "disabled"` |
| 6.4.2 | 用 `full_features` 固件（`=y`） | 拍照后 `[MQTT]` 有 publish meta 行；MQTT 失败**不影响** `ok_count` |

### 6.5 队列策略

| # | 操作 | 判据 |
|---|---|---|
| 6.5.1 | `max_queue_len=2`（最小 2），连拍超过容量 | 按 `queue_policy` 丢：`drop_count` 递增 |
| 6.5.2 | `GET /api/status` | `queue.len/bytes/mode/policy` 与实际一致 |

---

## 7. 触发 FSM

| # | 操作 | 判据 |
|---|---|---|
| 7.1 | 短按按键（**BOOT0 = GPIO0**） | `[LED ] button short press held=..ms -> 人脸识别（有脸才上传）`，随后见 `人脸检测 HIT/MISS`；HIT 才入队上传；**无脸时**打 `非人形帧丢弃` |
| 7.2 | 长按 5 s | `[TRIG] button long -> factory reset`，重启后配置回默认 |
| 7.3 | 触发 PIR | `[TRIG] pir -> fire` |
| 7.4 | 触发后 3 s 内再触发 | `[TRIG] cooldown, skip` |
| 7.5 | 撤防后触发 | `[TRIG] not armed, skip` |
| 7.6 | 1 s 内敲 20 次触发 | `[CAM ] grab` **不重叠**（一 grab 一 return 交替）← 并发判据 |

---

## 8. Web API

| # | 操作 | 判据 |
|---|---|---|
| 8.1 | `python tools/test_api.py --host 192.168.0.104` | 只读端点全 PASS |
| 8.2 | 加 `--key <admin_key>` | 需鉴权端点也 PASS |
| 8.3 | `POST /api/config` 不带 key | `401 {"error":"unauthorized"}` |
| 8.4 | `GET /api/config` | 密钥字段全是 `"***"`，看不到明文 |
| 8.5 | `GET /api/status` | 有 `wifi`/`camera`/`sd`/`queue`/`trigger`/`snapshot`/`sys` 七个子对象 |
| 8.6 | `GET /api/sd` | 同时有编译期 `mode` 与运行期 `state` |
| 8.7 | `GET /api/stream`（默认固件） | `501 {"error":"stream disabled in this build"}` |
| 8.8 | `GET /api/stream`（`full_features`） | `multipart/x-mixed-replace`，浏览器能看动态画面 |
| 8.9 | 不存在的路径 | `404` 且是 JSON，不是 HTML 错误页 |
| 8.10 | `GET /api/events` / `/api/log` | 顶层都是**数组**；分别最多 50 / 80 条 |

---

## 9. OTA

| # | 操作 | 判据 |
|---|---|---|
| 9.1 | `POST /api/ota` 不带 key | `401` |
| 9.2 | 带 key + 正确 MD5 | `[OTA ] download ... md5 ok` → `writing ...` → 重启 |
| 9.3 | 重启后 | `[OTA ] last_result=ok`，版本号更新 |
| 9.4 | **故意给错 MD5** | `[OTA ] md5 mismatch ... abort`，**不写分区、不重启** |
| 9.5 | 9.4 之后 | 仍运行旧固件，功能正常（**无 bootloop**） |
| 9.6 | 中途断电 | 下次启动回滚旧分区，`[OTA ] last_result=fail` |
| 9.7 | `CONFIG_SNAP_ENABLE_OTA=n` 固件 | `501 {"error":"ota disabled in this build"}` |

> 关键设计：**先下到 PSRAM 算 MD5，校验通过才写分区** → 错误 MD5 不会产生 bootloop。

---

## 10. LED 与日志（含本轮修复验证）

### 10.1 LED 语义

| 状态 | 现象 |
|---|---|
| 启动中 | WS2812 红闪 |
| 就绪 | 绿灯慢闪 |
| 拍照中 | 短暂变化后回就绪 |
| 上传成功 | 变化指示 |
| 致命 | **红灯常亮**（REQUIRED 无卡 / camera 失败） |

### 10.2 日志互锁 —— ★ 本轮重点验证

| # | 操作 | 判据 |
|---|---|---|
| 10.2.1 | 看 `[SYS ] heap=... psram=... armed=1 upload=buffer` | **完整一行**，不被 `cam_hal:` 等驱动日志劈开 |
| 10.2.2 | 看 `I (17xx) wifi:...` | 时间戳与消息都完整，无 `I (17OI - JPEG sta33) wifi:...` 这类交错 |
| 10.2.3 | 格式核对 | 每行 `L (t_ms) [TAG  ] message`，TAG 5 字符左对齐 |

> ⚠️ 上电首帧偶发 1~2 条 `cam_hal: NO-SOI - JPEG start marker missing`
> 属**摄像头首帧垃圾数据**，正常；**只有持续刷屏才是故障**。

### 10.3 日志工具

| # | 操作 | 判据 |
|---|---|---|
| 10.3.1 | `python tools/parse_log.py build_log_serial.txt` | 打印 TAG 计数，无未知 TAG 警告 |
| 10.3.2 | 加 `--check` | 只输出验收结论（boot/camera/sd/wifi/ready） |
| 10.3.3 | 全日志搜密钥 | **搜不到**完整 token / admin_key / Wi-Fi 密码 |

> `parse_log.py` 只有 `--check` / `--tag` / `--level` / `--timeline` / `--grep`，
> **没有** `--summary` / `--check-fb-leak`。

---

## 11. 配置与持久化

| # | 操作 | 判据 |
|---|---|---|
| 11.1 | `POST /api/config` 改 `frame_size` | 返回 `{"ok":true}`；**改完必须 `GET /api/config` 复核**（非法值会被静默忽略但仍返回 ok） |
| 11.2 | 改 `frame_size` 超型号上限 | `[CAM ] frame_size N > max M (OV2640) -> clamp` |
| 11.3 | 重启 | 配置保持（littlefs `/config.json`） |
| 11.4 | 长按 5 s / `POST /api/factory_reset` | 回默认，设备重启 |

---

## 12. 勘误：`testing.md` 中已失效的条目

照旧文档做会得到**假失败**，逐条对照如下：

| 旧文档写法 | 实际（当前代码） |
|---|---|
| `[UPLD] http POST ... status=200 ok` | TAG 是 **`UPLOAD`**；无 `UPLD` 这个 TAG |
| `[UPLD] telegram sendPhoto ok` | TAG 是 **`TG  `** |
| `[SD] mount fail ... retry=30s`（固定 30 s） | 已改**指数退避封顶 10 min**：30→60→120→…→600 s |
| 4.1.5 「重新插卡等 30 s」 | 退避期间最长可能要等 **10 分钟**；急用请调 `POST /api/sd/remount` |
| §2 未提"已配网是否开热点" | 已配网**不再开热点**；仅配网时开，完成后 3 s 自动关闭 |
| §5.3「3 次都失败 → 进入重试队列」 | 无通道时**不记为失败**，打 `no channel configured`，`fail_count` 不变 |
| `[SD] remount ok recovered state=OK` | 仍准确（少一个词 grep 不到这条注意保留） |
| `[WIFI] ...` 单括号 | 实际是 `[WIFI ]`（5 字符左对齐） |

> 改完代码同步改文档，是本工程的既有约定（见根 `AGENTS.md` §6）。
