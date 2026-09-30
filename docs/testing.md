# 测试清单

> ⚠️ **本文件是首版设计态清单，部分条目已被后续代码改动推翻。**
> **以 [`testing_full.md`](testing_full.md) 为准**（含勘误对照表 §12）。
> 照旧写法会得到"假失败"的几处：`[UPLD]` TAG 不存在（应为 `UPLOAD` / `TG  `）、
> SD 已改指数退避、已配网不再开热点、无通道不再记 fail。

每条都给出**可验证判据**（日志关键字 + 屏幕/现象），不要只说"应该好了"。
串口命令用 `idf.py -p COM6 monitor`，退出 `Ctrl+]`。

---

## 0. 编译与烧录

| # | 操作 | 判据 |
|---|---|---|
| 0.1 | `powershell -File tools/build_matrix.ps1` | 末行 `RESULT: 6 / 6 configurations built OK` |
| 0.2 | 每组 `VERIFY:` 行 | `CONFIG_CAM_OV*` 与 `CONFIG_OV*_SUPPORT` **成对且只有一对** |
| 0.3 | `Select-String sdkconfig -Pattern '^CONFIG_OV\w+_SUPPORT=y'` | **只有一行** |
| 0.4 | `idf.py -p COM6 flash monitor` | `[BOOT] esp32s3-snapshot-kit v0.1.1 sensor=OV2640(0x2642) sd_mode=OPTIONAL` |
| 0.5 | 紧接第二行 | `[BOOT] idf=v5.5.1 chip=ESP32-S3 rev=N flash=16384KB psram=8192KB(OCT) reset=POWERON` |

---

## 1. 启动（不插卡）

| # | 操作 | 判据 |
|---|---|---|
| 1.1 | 拔掉 SD 卡，复位 | `[BOOT] ... sd_mode=OPTIONAL`，**不卡在挂载** |
| 1.2 | 观察 SD 日志 | `[SD] mount start clk=39 cmd=38 d0=40 mode=1bit` → `[SD] mount fail err=0x... -> degrade queue_mode=buffer retry=30s` |
| 1.3 | 观察启动是否继续 | 出现 `[READY]`，**没有** `fatal` / `safe mode` |
| 1.4 | 观察 LED | WS2812 启动期红闪 → 就绪后**绿灯慢闪** |
| 1.5 | 搜索 `safe mode` | **搜不到**（camera 正常时）；出现即说明进了降级固件模式 |

> **关键**：不插卡必须能走到就绪。这是任务书硬性要求，也是
> `ov2640_optional` 组要覆盖的主路径。

---

## 2. AP 配网

| # | 操作 | 判据（★ 日志文案已按 `wifi_hal.c` 实际打印核对） |
|---|---|---|
| 2.1 | 首次上电（无 Wi-Fi 配置） | `[WIFI] 未配置 wifi_ssid -> 直接进入 AP 配网` |
| 2.1b | 有配置但连不上 | `[WIFI] STA 60s 未连上 -> 进入 AP 配网 ssid=ESP32S3-Setup pass=12345678 ip=192.168.4.1` |
| 2.1c | 紧接一行 | `[WIFI] 请在浏览器打开 http://192.168.4.1 配置 Wi-Fi` |
| 2.2 | 手机扫描热点 | 看到 **`ESP32S3-Setup`** |
| 2.3 | 连接，密码 `12345678` | 连接成功，拿到 `192.168.4.x`；串口 `[WIFI] ap client joined <MAC>` |
| 2.4 | 浏览器 `http://192.168.4.1/` | 出现控制台页面 |
| 2.5 | 浏览器 `http://esp32s3cam.local/` | 同上（mDNS 生效） |
| 2.6 | 在页面填 Wi-Fi 并保存 | `{"ok":true}`；串口 `[WIFI] ap 配网凭据更新 -> 重连 ssid=...` |
| 2.7 | 重启后 | `[WIFI] connected ip=192.168.x.x rssi=-NN` |
| 2.8 | ★ 已配网时复位 | **不开热点**：搜不到 `ESP32S3-Setup`，无 `[WIFI] ap started` / `softAP` / `ap channel adjust` |
| 2.9 | ★ 配网保存成功后约 3 s | `[WIFI] AP 配网完成 -> 关闭热点，仅保留 STA`，热点消失、STA 保持 |

---

## 3. 拍照主链路

| # | 操作 | 判据 |
|---|---|---|
| 3.0 | `POST /api/arm` | `{"armed":true}`（**先布防，否则 3.1 会 409**） |
| 3.1 | `POST /api/snapshot` | `{"accepted":true,"force":false}` |
| 3.2 | 串口（随后异步出现） | `[CAM ] grab ...` + `[PIPE]` / `[Q   ]` 的入队行 |
| 3.3 | 连点 5 次 | 第 2 次起 `409 {"error":"cooldown"}` |
| 3.4 | `POST /api/snapshot?force=1` | `{"accepted":true,"force":true}`，绕过冷却 |
| 3.4b | 撤防后 `?force=1` | 仍 `409 {"error":"disarmed"}` ← **force 绕不过布防** |
| 3.5 | `GET /api/last_snapshot` | 返回 JPEG，能在浏览器直接看到图 |
| 3.6 | 拔卡后再拍 | 仍 `accepted:true`；`[SYS]` 无 `[CAM] grab failed` |
| 3.7 | 连拍 50 次（force） | `GET /api/status` 的 `snapshot.psram_min` **稳定**，不持续下降 |
| 3.8 | 同上 | `sys.free_heap`、`sys.free_psram` 无单调下降趋势 |

**无泄漏判据（两条，都要看）**：

1. `[CAM ]` 的 `grab` 与 `return` 行数**必须配对** ——
   `grab` 比 `return` 多就说明某条路径漏了 `esp_camera_fb_return()`；
2. `snapshot.psram_min` 在 50 次连拍过程中**不单调下降**。

统计各 TAG 行数：

```powershell
python tools/parse_log.py build_log_serial.txt
```

---

## 4. SD 三态

### 4.1 OPTIONAL（默认）

| # | 操作 | 判据 |
|---|---|---|
| 4.1.1 | 插卡启动 | `[SD] mount ok total=NNNMB free=NNNMB cid=...`，状态 `SD_OK` |
| 4.1.2 | 拍照 | 文件出现在 `/sdcard/snapshots/YYYY/MM/DD/` |
| 4.1.3 | 运行中拔卡 | 连续 3 次写失败后 `[SD] write fail count>=3 state=FAIL -> degrade` |
| 4.1.4 | 拔卡后拍照 | 仍成功（走 PSRAM 队列），`queue.mode` 变 `buffer` |
| 4.1.5 | 重新插卡等退避周期 | `[SD] retry mount start (attempt N)` → `[SD] remount ok recovered state=OK`（★ 退避已改**指数封顶 10 min**，急用请调 `POST /api/sd/remount`，别干等） |
| 4.1.6 | `POST /api/sd/remount` | `[SD] manual remount requested`，返回 `{"ok":true,...}`（需 `X-Admin-Key`） |

### 4.2 NONE

| # | 操作 | 判据 |
|---|---|---|
| 4.2.1 | 用 `ov2640_sd_none` 固件启动 | `[BOOT] ... sd_mode=NONE` |
| 4.2.2 | 紧接一行 | `[SD] mode=NONE -> sd_hal disabled (all calls return NOT_SUPPORTED)` |
| 4.2.3 | `GET /api/sd` | `"mode":"none"` 且 `"state":"SD_DISABLED"` |
| 4.2.4 | 插卡也没用 | 仍然 `SD_DISABLED`（编译期关掉） |
| 4.2.5 | 拍照 | 成功，全部走 PSRAM 队列 |

### 4.3 REQUIRED

| # | 操作 | 判据 |
|---|---|---|
| 4.3.1 | **不插卡**启动 | `[SD] mount fail err=0x... ` → `[SD] mode=REQUIRED and mount failed -> fatal` |
| 4.3.2 | LED | **红灯常亮**（`LED_ST_FATAL`） |
| 4.3.3 | web | 仍能进配置页（safe mode 保留 web） |
| 4.3.4 | `POST /api/snapshot` | `503 {"error":"safe mode: camera pipeline disabled"}` |
| 4.3.5 | 插卡重启 | `[SD] mount ok ...`，正常进 `[READY]` |

---

## 5. 上传

> ★ **勘误（见 `testing_full.md` §12）**：TAG 不是 `[UPLD]`，HTTP 是 **`[UPLOAD]`**、
> Telegram 是 **`[TG  ]`**；另外"没配任何出口"**不再记为失败**（打
> `[Q   ] no channel configured ...`，`fail_count` 不变）。

| # | 操作 | 判据 |
|---|---|---|
| 5.1 | 配好 `http_url`，拍照 | `[UPLOAD] http POST ... status=200 ok` |
| 5.2 | 故意配错 URL | 出现 `[UPLOAD]` 的 `retry` 行，间隔依次约 1 s / 2 s / 4 s（退避） |
| 5.3 | 3 次都失败 | 进入重试队列，**不阻塞后续拍照** |
| 5.4 | 配 Telegram token+chat | `[TG  ] telegram sendPhoto ok` |
| 5.5 | token 打印 | 日志里**只出现前 6 位**，如 `123456***` |
| 5.6 | `GET /api/status` 队列字段 | `queue.len` / `cap` / `policy` 与实际一致 |
| 5.7 | ★ 不配任何出口，触发拍照 | `[Q   ] no channel configured ...`，且 `fail_count` **不增长** |

---

## 6. 触发 FSM

| # | 操作 | 判据 |
|---|---|---|
| 6.1 | 短按按键（**BOOT0 = GPIO0**） | `[LED ] button short press held=..ms -> 人脸识别（有脸才上传）`，随后见 `人脸检测 HIT/MISS`；HIT 才入队上传 |
| 6.2 | 长按 5 s | `[TRIG] button long -> factory reset`，重启后配置回默认 |
| 6.3 | 触发 PIR（GPIO1） | `[TRIG] pir -> fire` |
| 6.4 | 触发后 3 s 内再触发 | `[TRIG] cooldown, skip`（默认 `min_trigger_interval_ms=3000`） |
| 6.5 | 撤防后触发 | `[TRIG] not armed, skip` |
| 6.6 | `?force=1` 触发 | 绕过 armed 与冷却 |

**★ 并发判据**（任务书的"连拍 50 次"）：冷却判断必须在**串行 trigger 任务**里，
否则并发拍照会同时抢 framebuffer。

| 6.7 | 1 s 内敲 20 次触发 | 日志里 `[CAM] grab` **不重叠**（一 grab 一 return，交替出现） |

---

## 7. Web API

| # | 操作 | 判据 |
|---|---|---|
| 7.1 | `python tools/test_api.py --host 192.168.4.1` | 全部只读端点 PASS |
| 7.2 | `python tools/test_api.py --host 192.168.4.1 --key K` | 需鉴权端点也 PASS |
| 7.2b | `python tools/test_api.py --host 192.168.4.1 --burst 50` | 连拍 50 次，脚本自报内存验收结论 |
| 7.3 | `POST /api/config` **不带** key | `401 {"error":"unauthorized"}` |
| 7.4 | `POST /api/config` 带错 key | 同上 401 |
| 7.5 | `GET /api/config` | 密钥字段全是 `"***"`，**看不到明文** |
| 7.6 | `GET /api/status` | 有 `wifi`/`camera`/`sd`/`queue`/`trigger`/`snapshot`/`sys` 七个子对象 |
| 7.6b | `GET /api/sd` | 同时有 `mode`（编译期）与 `state`（运行期） |
| 7.7 | `GET /api/stream`（默认固件） | `501` |
| 7.8 | `GET /api/stream`（`full_features` 固件） | `Content-Type: multipart/x-mixed-replace`，浏览器能看动态画面 |
| 7.9 | 不存在的路径 | `404` 且是 JSON，不是 HTML 错误页 |
| 7.10 | `GET /api/photos` | 顶层是**数组** `[...]`，不是 `{"files":[...]}` |
| 7.11 | `GET /api/events`、`GET /api/log` | 同样顶层是**数组** |

---

## 8. OTA

| # | 操作 | 判据 |
|---|---|---|
| 8.1 | 准备好 https 固件 URL 与 MD5 | — |
| 8.2 | `POST /api/ota`（不带 key） | `401` |
| 8.3 | `POST /api/ota`（带 key，正确 MD5） | `[OTA] download ... md5 ok` → `[OTA] writing ...` → 重启 |
| 8.4 | 重启后 | `[OTA] last_result=ok`，版本号已更新 |
| 8.5 | **故意给错 MD5** | `[OTA] md5 mismatch ... abort`，**不写分区、不重启** |
| 8.6 | 8.5 之后设备状态 | 仍运行旧固件，功能正常 |
| 8.7 | 断电模拟升级失败 | 下次启动回滚到旧分区，`[OTA] last_result=fail` |

> 关键设计：**先下载到 PSRAM 算 MD5，校验通过才写分区**。
> 所以 8.5 不会产生 bootloop（任务书硬性要求）。

---

## 9. LED 语义

| 状态 | 现象 |
|---|---|
| 启动中 | WS2812 红闪 |
| 就绪 | 绿灯慢闪 |
| 拍照中 | 短暂变化后回就绪 |
| 上传中 | 变化指示 |
| 致命 | **红灯常亮**（REQUIRED 无卡 / camera 失败） |

---

## 10. 日志规范

| # | 操作 | 判据 |
|---|---|---|
| 10.1 | `python tools/parse_log.py build_log_serial.txt` | 打印归一化结果 + 各 TAG 计数，无未知 TAG 警告 |
| 10.1b | `python tools/parse_log.py build_log_serial.txt --check` | 只输出**验收判据**结论（boot/camera/sd/wifi/ready） |
| 10.1c | `python tools/parse_log.py build_log_serial.txt --tag SD CAM` | 只看这两个 TAG |
| 10.2 | 检查格式 | 每行都是 `L (t_ms) [TAG  ] message`（TAG 5 字符左对齐，**不是** `[TAG]` 紧贴） |
| 10.3 | 搜密钥 | 全日志**搜不到**完整 token / admin_key / Wi-Fi 密码 |

> `parse_log.py` **没有** `--summary` / `--check-fb-leak` 这类参数；
> 可用的只有 `--check` / `--tag` / `--level` / `--timeline` / `--grep`。
> 还可以当管道用：
> `python -m serial.tools.miniterm COM6 115200 | python tools/parse_log.py -`

TAG 清单以 `main/logger.h` 的 `LOG_T_*` 为准：`BOOT` `CFG` `WIFI` `CAM` `SD`
`TRIG` `PIPE` `Q` `UPLOAD` `TG` `MQTT` `WEB` `OTA` `SYS` `LED`
（**没有** `UPLD` 和 `HUMAN`）。

---

## 11. 交付物核对

- [ ] 完整工程树（`main/` 全部源文件 + `web/index.html`）
- [ ] `build_log_<组名>.txt` × 6，末行都是 6/6
- [ ] `README.md`
- [ ] `docs/wiring.md` `sensors.md` `sd_modes.md` `api.md` `testing.md` `troubleshooting.md` `debug_notes.md`
- [ ] `tools/test_api.py` `tools/parse_log.py`
- [ ] `tools/build_matrix.ps1` + `tools/cfg_overrides/`
