# HTTP API（任务书 5.7）

基地址：`http://192.168.4.1/`（AP 模式）或 `http://esp32s3cam.local/`（STA 模式，mDNS）。

所有 JSON 响应都是：

```http
Content-Type: application/json; charset=utf-8
```

所有错误响应统一为：

```json
{ "error": "<reason>" }
```

并带对应的 HTTP 状态码（400 参数错 / 401 未授权 / 404 找不到 / 405 方法不许 / 500 内部错）。

---

## 1. 鉴权

需要 `X-Admin-Key: <admin_key>` 的端点（`admin_key` 来自 `/config.json`）：

- `POST /api/config`
- `POST /api/ota`
- `POST /api/factory_reset`
- `POST /api/sd/remount`

不符时返回：

```http
HTTP/1.1 401 Unauthorized
{"error":"unauthorized"}
```

只读端点、拍照、`arm`/`disarm` **不要求**鉴权 —— 局域网内便于配网后直接验证。
若要全端点鉴权，改 `web_server.c` 的 `require_admin()` 调用点即可。

---

## 2. 端点总览

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/` | — | 内嵌单页控制台（`web/index.html`） |
| GET | `/favicon.ico` | — | 内嵌图标，避免 404 刷屏 |
| GET | `/api/status` | — | 设备状态（见下） |
| POST | `/api/snapshot` | — | 立刻拍一张；`?force=1` **只是语义标注，不绕冷却、也不绕 armed**（见本节末尾的说明） |
| GET | `/api/last_snapshot` | — | 最近一张 JPEG（`image/jpeg`） |
| GET | `/api/photos` | — | 文件列表（SD_OK 才有内容） |
| GET | `/api/events` | — | 最近 50 条事件（内存环形表） |
| GET | `/api/log` | — | 最近 80 行日志（内存环形缓冲） |
| GET | `/api/config` | — | 脱敏配置 |
| POST | `/api/config` | ✅ | 改配置（部分键即可） |
| POST | `/api/arm` | — | 布防 |
| POST | `/api/disarm` | — | 撤防 |
| GET | `/api/sd` | — | SD 状态（含**编译期 mode** 与运行期 state） |
| POST | `/api/sd/remount` | ✅ | 手动重挂载 |
| GET | `/api/stream` | — | MJPEG（仅 `CONFIG_SNAP_ENABLE_STREAM=y`，默认关） |
| POST | `/api/ota` | ✅ | 触发 OTA（`url` 必填，`md5` 可选） |
| POST | `/api/reboot` | — | 重启 |
| POST | `/api/factory_reset` | ✅ | 恢复出厂并重启 |

---

## 3. `GET /api/status`

返回**嵌套**结构（顶层 6 个字段 + 6 个子对象）。下面是实测形状：

```json
{
  "device_id": "s3cam-6f3a",
  "fw_version": "0.1.1",
  "uptime_ms": 1234567,
  "armed": true,
  "safe_mode": false,

  "wifi": { "mode": "STA", "ip": "192.168.0.123", "ssid": "MyHome", "rssi": -58 },

  "camera": {
    "sensor": "OV2640",
    "expected_pid": "0x2642",
    "pid": 9794,
    "framesize": 10,
    "framesize_name": "SVGA(800x600)",
    "jpeg_quality": 12
  },

  "sd": { "state": "SD_FAIL", "total_kb": 0, "free_kb": 0, "retry_count": 3, "cid": "" },

  "queue": {
    "len": 2, "bytes": 366408, "mode": "buffer", "policy": "drop_oldest",
    "ok_count": 17, "fail_count": 0, "drop_count": 0, "last_error": ""
  },

  "trigger": {
    "state": "idle", "in_cooldown": false, "count": 12,
    "cooldown_skips": 5, "min_interval_ms": 3000
  },

  "snapshot": { "ok_count": 12, "fail_count": 0, "psram_min": 3145728 },

  "sys": {
    "free_heap": 189432, "free_psram": 4194304,
    "time_valid": true, "time": 1758880801, "tz": "CST-8",
    "upload_mode": "http", "mqtt": "disabled", "led": "normal",
    "ota_in_progress": false, "mdns": "esp32s3cam.local"
  }
}
```

要点：

- `sd.state` 是**运行期**状态（`SD_OK` / `SD_FAIL` / …）；
  **编译期**的 `NONE`/`OPTIONAL`/`REQUIRED` 不在这个接口里，
  它体现在启动 banner 的 `sd_mode=` 与 `GET /api/sd` 上；
- `snapshot.psram_min` 是**连拍泄漏验收**的关键指标 —— 反复 force 拍照时它应保持稳定；
- `queue.mode` 是 `path`（有卡）或 `buffer`（降级到 PSRAM）；
- `sys.led` 只区分 `fatal` / `normal`。

完整字段名请对照 [`main/web_server.c` 的 `h_status()`](
../main/web_server.c)。

---

## 4. 拍照与取图

```http
POST /api/snapshot
```

成功（事件已被 FSM 接受，**拍照与上传是异步进行的**，所以这里不返回 path）：

```json
{ "accepted": true, "force": false }
```

想拿文件，请随后调 `GET /api/last_snapshot` 或 `GET /api/photos`。

### 错误码（实测）

| 状态 | body | 触发条件 |
|---|---|---|
| `503` | `{"error":"safe mode: camera pipeline disabled"}` | camera / REQUIRED-SD 失败进的 safe mode |
| `409` | `{"error":"disarmed"}` | 未布防（**`force=1` 也绕不过这一条**） |
| `409` | `{"error":"cooldown"}` | 被 `min_trigger_interval_ms` 冷却拦下 |
| `409` | `{"error":"busy"}` | 触发队列满 / 上一次还没处理完 |

> ★ `?force=1` 的语义**只是"把这次触发标记为强制"**（作为 note 透传给 FSM，
>   日志里显示 `note=http-force`）。**它既不能绕过 armed，也不能绕过冷却**：
>   - `armed` 在 web 层就检查并返回 409 了（`h_snapshot_post`）；
>   - 冷却门在 `trigger_fsm.c` 的 `gate_and_capture()` 里，`force` 根本不参与判断
>     （源码注释原话：*"绕过冷却由 trigger_fsm 的 min_interval 控制，这里只作为日志/语义标注"*）。
>
>   ⇒ **连拍验收时，两次 `/api/snapshot` 之间必须空出 `min_trigger_interval_ms`
>   （默认 3000ms）**，否则第二次拿到的是 `409 cooldown`。
>   这一点极易被误判成"拍照失败了"—— 实际是**被冷却正确拦下**，
>   串口日志里对应 `[TRIG ] cooldown skip count=.. src=remote_http (..ms < 3000ms)`。

---

## 5. 配置读写

`GET /api/config` 返回**脱敏**版本 —— 密钥字段替换为 `"***"`：

```json
{
  "device_id": "s3cam-6f3a",
  "wifi_ssid": "MyHome",
  "wifi_pass": "***",
  "admin_key": "***",
  "telegram_token": "***",
  "armed": 1,
  "upload_mode": "http",
  "jpeg_quality": 12,
  "frame_size": 10,
  "min_trigger_interval_ms": 3000,
  "queue_policy": "drop_oldest",
  "max_queue_len": 8,
  "sd_low_space_mb": 200,
  "timezone": "CST-8"
}
```

`POST /api/config` 只需给要改的键，**未出现的键不变**：

```json
{ "wifi_ssid": "NewSSID", "wifi_pass": "newpass", "frame_size": 12 }
```

返回 `{ "ok": <esp_err_t 是否为 ESP_OK> }`：

```json
{ "ok": true }
```

> ⚠️ 返回**不含** `changed` / `ignored` / `reboot_needed` 列表。
> 想知道到底改了什么，请改完再 `GET /api/config` 对比。
> 非法值（超范围/类型错）由 `config_store` 在 apply 阶段打 warning 并忽略该键，
> **接口层仍可能返回 `ok:true`** —— 别只看 HTTP 结果就认为全部生效。

其它错误：

| 状态 | body |
|---|---|
| `401` | `{"error":"unauthorized"}` |
| `413` | `{"error":"body empty or too large"}` |
| `400` | `{"error":"invalid json"}` |

> 脱敏只发生在"序列化给 web 的那一份"（`config_store_to_json(true)`），
> 内存里的真实值不动，所以改配置时不用先回填密钥。

---

## 6. 其它

### ★ 返回"裸数组"的三个端点

`/api/photos`、`/api/events`、`/api/log` **返回的是顶层 JSON 数组**，
不是 `{"count":N,"items":[...]}` 这种包装对象。写客户端时别套错一层：

`GET /api/photos` → `200`

```json
[
  { "name": "192001_012.jpg", "size": 183204, "path": "snapshots/2026/09/26/192001_012.jpg" }
]
```

`GET /api/events` → `200`（最近 **50** 条，内存环形表，最新在末尾）

```json
[
  { "ts": 1758880801, "kind": "pir", "result": "queued", "path": "snapshots/..." }
]
```

`GET /api/log` → `200`（最近 **80** 行，内存环形缓冲，最新在末尾）

```json
[ "[BOOT] esp32s3-snapshot-kit v0.1.1 ...", "[SD] mount fail -> degrade ...", "[READY]" ]
```

> 这三个端点在缓冲区不够时会**截断数组**（而不是返回 500），
> 所以拿到的是"最近 N 条里能装下的部分"，不是错误。
> 结构实现见 `upload_queue_events_json()`（`upload_queue.c`）
> 与 `logger_dump_json()`（`logger.c`）。

### `GET /api/sd`（`200`）

唯一**同时**暴露编译期 mode 与运行期 state 的端点：

```json
{
  "state": "SD_FAIL",
  "mode": "optional",
  "total_kb": 0,
  "free_kb": 0,
  "retry_count": 3,
  "last_error": 0,
  "cid": ""
}
```

- `mode` 是**编译期**三态，值域 `none` / `optional` / `required`（全小写）；
- `state` 是**运行期**状态（`SD_OK` / `SD_FAIL` / `SD_DISABLED` / …）；
- `cid` 是卡的 CID 字符串，无卡时为空串。

### `POST /api/sd/remount`（`200`）

```json
{ "ok": true, "err": 0, "state": "SD_OK" }
```

`err` 是原始 `esp_err_t` 数值，排查时比 `ok:false` 有用得多。

`POST /api/ota`（**先下载到 PSRAM 算 MD5，校验通过才写分区**，失败不重启）：

```json
{ "url": "https://example.com/fw.bin", "md5": "d41d8cd98f00b204e9800998ecf8427e" }
```

实测错误码：

| 状态 | body |
|---|---|
| `501` | `{"error":"ota disabled in this build"}`（`CONFIG_SNAP_ENABLE_OTA=n`） |
| `401` | `{"error":"unauthorized"}` |
| `409` | `{"error":"ota already in progress"}` |
| `400` | `{"error":"url required"}` |

`GET /api/stream` 在 `CONFIG_SNAP_ENABLE_STREAM=n`（默认）时返回
`501 Not Implemented`。

---

## 7. 自测

主机用 `--host`（**不是位置参数**），密钥用 `--key`：

```powershell
python tools/test_api.py --host 192.168.4.1                  # 只读端点
python tools/test_api.py --host 192.168.4.1 --key <admin_key> # 含需鉴权端点
python tools/test_api.py --host esp32s3cam.local
python tools/test_api.py --host 192.168.4.1 --burst 50       # 连拍 50 次做内存验收
python tools/test_api.py --host 192.168.4.1 --only sd        # 只跑名字含 sd 的用例组
```

脚本只用标准库（`urllib`），**不需要 pytest、不需要装依赖**。
