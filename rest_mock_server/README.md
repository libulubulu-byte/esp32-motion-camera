# REST 测试后台（配合固件 REST 后端）

给 `AWS_mqtt_REST_API` 工程的 REST 分支提供服务器端：接收设备上报、下发灯命令、
并在浏览器里显示实时数据、历史曲线和灯开关。

不需要 Docker，不需要云服务，Windows 上双击 bat 就能跑。

## 一键启停

| 脚本 | 作用 |
| --- | --- |
| `start.bat` | 启动服务，自动查本机 IP、自动缺啥装啥、自动开浏览器 |
| `stop.bat` | 停止服务（先按 PID 杀，再按端口兜底扫） |
| `status.bat` | 看进程/端口/HTTP 是否正常 |
| `test_api.bat` | 模拟设备跑一遍全部接口，验证服务端协议是否正确 |

双击 `start.bat` 即可。首次运行会自动 `pip install flask`。

服务在后台独立运行，关掉 bat 窗口不影响；要停就双击 `stop.bat`。

### ★ 端口：本服务用 8081（不是 8080）

| 端口 | 程序 | 干什么 |
|---|---|---|
| **8080** | `..\tools\recv_server.py` | 收 `esp32S3_CAM` 上传的照片（极简接收端，只用标准库） |
| **8081** | **本服务** `server.py` | `/ota.json` + `/firmware/*` + REST 仪表盘 |

`start.bat` 与 `run_all.bat` 里都已写死 `set "PORT=8081"`（2026-09-27 从 8080 改过来）。

**为什么必须分开**：`recv_server.py` 对**所有** GET 路径都回
`{"server":"alive","hint":"POST multipart to this URL"}`。设备的升级清单地址一旦被指到 8080，
它拿到的就是这份"没有 version 字段的清单"，串口打
`[OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过` —— 看起来像 OTA 坏了，其实是端口被抢答。

> 附带一条 Windows 特有的坑：**同一端口允许被两个进程同时 `LISTENING`**（都设了
> `SO_REUSEADDR` 时）。所以 `netstat` 里有一行 LISTENING **不代表**只有一个进程在监听，
> 会造成"有时拿到 alive、有时拿到真清单"的飘忽现象。
> 排查与按 PID 杀进程的方法见 [`../docs/debug_notes.md`](../docs/debug_notes.md) §5.4。

`run_all.bat` = **停旧服务 → 构建 → 重启服务 → 自检** 一条龙；
只想重启服务、不动固件就加 `-nobuild`。

## 设备侧要填的配置

`start.bat` 会直接打印出来，对照填进设备配网页（SoftAP 热点 → 打开配网页面）：

| 配网页字段 | 值 | 说明 |
| --- | --- | --- |
| `backend` | REST API | 默认就是这个 |
| `rest_host` | 如 `192.168.0.101` | 电脑的局域网 IP，**不能填 localhost** |
| `rest_port` | `8081` | |
| `rest_path` | `/report` | 上报接口 |
| `rest_cmd` | `/lampcmd` | 灯命令轮询接口 |
| `rest_tls` | 关 | 明文 HTTP，避免自签证书的坑 |

改端口的话同时改 `server.py` 的 `PORT`/环境变量和设备配网页，两边要一致。
路径可以用环境变量覆盖：`REPORT_PATH` / `CMD_PATH`。

> **上报路径别名（2026-09-27）**：服务器同时接受 `POST /report` 和 `POST /upload`。
> 设备的上报地址存在自己的 NVS 里（配网页填的 `http_url`），如果它填的是
> `/upload` 而服务器只认 `/report`，现象是固件日志里一串
> `[UPLOW] http fail err=http_status_404`，重试 3 次后**照片被丢弃**
> （`[Q   E] upload fail ... 已丢弃本次事件`），看起来像"上传功能坏了"。
> 现在两个名字都收，避免这种配置不一致。要退回严格单路径：
> `REPORT_ALIASES=""`。

## 硬件接线与灯

灯的命令是**轮询式**的：设备每 1 秒（`APP_REST_POLL_INTERVAL_MS`）GET 一次
`/lampcmd`。你在控制台点「开灯」，命令会排队，设备下一轮取走后立即清除，
取走后设备会马上补报一次状态，所以 UI 上能立刻看到反馈。

## 接口说明

设备侧（固件调用的）：

```
POST /report    体: {"device","version","temperature","humidity","rssi","uptime","lamp"}
                响应: {"ok":true}   只有 2xx 才会被固件认作成功

GET  /lampcmd   响应: {"lamp":"ON"} / {"lamp":"OFF"} / 204 No Content
                204 表示"没有命令"，设备什么都不做
                固件也认 cmd / value / state 三个键名
```

控制台侧（浏览器调用的）：

```
GET  /                 仪表盘页面
GET  /api/latest       最新一条上报
GET  /api/history?limit=200
GET  /api/stats        计数 + 最后上报时间
POST /api/lamp         {"state":"ON"|"OFF"|"NONE"}  排队一条命令
POST /api/clear        清空历史
```

## 数据与日志

- `telemetry.db` — SQLite，历史数据。删掉它就等于重置。
- `server.log` — 服务端 stdout，包含每条上报和每个下发的命令。
- `server.pid` — 运行中的 PID，`stop.bat` 靠它精确停止。

## 常见问题

**start.bat 说端口被占用**
```
netstat -ano | findstr :8081
taskkill /F /PID <上面查到的PID>
```

**浏览器打不开但设备能上报**
`localhost:8081` 只能本机访问，检查防火墙是否拦了 8081 的入站；
设备走的是局域网 IP，需要放行。

**设备串口报 `POST ... failed` / `HTTP 000`**
先确认 `rest_host` 是不是电脑的真实局域网 IP。用手机浏览器访问
`http://<那个IP>:8081/` 试一下——手机能打开但设备不能，就是防火墙的问题。

## 固件托管（给 `esp32S3_CAM` 做 OTA 用）

本服务同时托管固件二进制，供设备走 `POST /api/ota` 拉取升级。

**为什么放在这里**：设备只认「一个能 GET 到 `.bin` 的 HTTP 地址」，
不需要额外的 JSON 清单协议，所以这个后台顺手就能当 OTA 服务器，
不用另起 nginx / python -m http.server。

### 接口

```
GET /firmware/              目录列表（浏览器/手机可看）
GET /firmware/manifest.json 最新 .bin 的 {"file","md5","size"}，用于取 MD5
GET /firmware/<name>.bin    固件本体（application/octet-stream，支持 Range）
```

`manifest.json` 取的是**目录里 mtime 最新的** `.bin`。

### 用法

```bat
:: 1. 构建固件
cd ..\..
idf.py build

:: 2. 发布（自动拷到 firmware\ 并算 MD5，顺带打印 OTA 命令）
cd rest_mock_server
python publish_firmware.py

:: 3. 触发设备升级（<pc-ip> 换成电脑局域网 IP）
curl -X POST "http://<pc-ip>/api/ota?url=http://<pc-ip>:8081/firmware/esp32s3_snapshot_kit.bin"
```

产物名跟 `main/CMakeLists.txt` 里的 `project(...)` 一致，默认是
`build\esp32s3_snapshot_kit.bin`。改了工程名的话，脚本会回退到
"build 下唯一的 `.bin`"，不会静默取错。

`publish_firmware.py` 也可以指定别的产物或改名，便于区分版本：

```bat
python publish_firmware.py ..\build\esp32s3_snapshot_kit.bin --name v1.2.3.bin
```

### 关于 MD5

`POST /api/ota` 的 `md5` 参数**可选**。不传时设备仍会在写完之后用
image header 校验兜底（`esp_ota_end`），但那是**下载完 3MB 之后**才知道；
传了 MD5 则**在写 flash 之前**就能发现传输损坏。建议用 `manifest.json`
或 `publish_firmware.py` 打印出的值。

### 这个后台与 `esp32S3_CAM` 的关系

上半部分（`/report`、`/lampcmd`、仪表盘）是 **`AWS_mqtt_REST_API` 工程**的
配套，与 `esp32S3_CAM` **没有任何关系** —— 那个工程用 REST 上报遥测并
轮询灯控命令，而 `esp32S3_CAM` 走的是 MQTT 上报 + 拍照归档，两者协议不同。

共用这个目录只是为了少开一个端口。给 `esp32S3_CAM` 做 OTA 时，
只需要 `/firmware/*` 这几条路由。
