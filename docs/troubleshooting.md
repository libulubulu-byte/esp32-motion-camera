# 排错：现象 → 根因 → 处理

本页所有条目都是**在本机实测踩过并已定位**的，不是泛泛的猜测。

---

## 1. 编译期

### `error: 'FRAMESIZE_3MP' undeclared`

**根因**：esp32-camera 2.1.7 的 `framesize_t` 里**没有 `FRAMESIZE_3MP`**。

**处理**：OV3660 的 3 MP 对应 **`FRAMESIZE_QXGA`**（2048×1536）。
注意 `FRAMESIZE_P_3MP` 是 864×1536（竖屏），那是另一个东西。

```c
#define SENSOR_MAX_FS      FRAMESIZE_QXGA   /* OV3660 3MP，不是 FRAMESIZE_3MP */
```

---

### `error: duplicate case value`（camera_hal.c 的 framesize 名字表）

**根因**：往 `camera_hal_framesize_name()` 的 `switch` 里补 case 时，
原本末尾已经有 `QHD/WQXGA/P_FHD/QSXGA` 四条，重复添加就撞了。

**处理**：补之前先读一遍那个 `switch` 的完整内容。
`-Werror` 会把 warning 升级成 error，所以这类错误一定是硬错误。

---

### `UnicodeEncodeError: 'gbk' codec can't encode character '\ufffd'`

**根因**：本机 Python 3.13 默认 `stdout` 编码是 **GBK（cp936）**。
`idf.py` 的 `read_and_write_stream()` 会把 cmake/kconfig 的输出原样
`output_converter.write()` 到 stdout；只要子进程输出了 GBK 无法表示的字符
（例如路径或工具打印的 `\ufffd`），整条构建就崩。

**处理**：把 Python 的 stdio 钉成 UTF-8。在 `tools/build_matrix.ps1` 里已经加了：

```powershell
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
```

`PYTHONIOENCODING` 是**关键**那一行（只设 `PYTHONUTF8` 不够，
因为 stdout 已经被 PowerShell 管道接管）。

---

### `project build complete` 有，但脚本说 FAIL

**根因**：脚本找错了产物名。工程名是 `project(esp32s3_snapshot_kit)`，
产物是 `build/esp32s3_snapshot_kit.bin`，**不是**目录名 `esp32S3_CAM.bin`。

**处理**：用产物名 + 成功标志双重判据：

```powershell
$bin = Join-Path $root "build\esp32s3_snapshot_kit.bin"
```

**别**用 `grep error` 当失败判据 —— 源码里的中文字符串会被按 GBK 解码成乱码，
误命中 `UnicodeEncodeError` 之类字面量。

---

### `Compilation failed because mqtt_hal.c (in "main" component) includes mqtt_client.h, provided by mqtt component(s)`

**根因**：IDF 5.5 的组件依赖检查是**无条件**的，它**不看** `mqtt_hal.c` 里
`#include "mqtt_client.h"` 外层有没有 `#if CONFIG_SNAP_ENABLE_MQTT`。

**处理**：`main/CMakeLists.txt` 的 `PRIV_REQUIRES` 里**无条件**列上 `mqtt`。
即使默认 `CONFIG_SNAP_ENABLE_MQTT=n` 也必须列。

---

### `unknown kconfig symbol 'XXX'`

**根因**：`sdkconfig.defaults` 里写了不存在的键。无害（只生成一条空注释），
但**危险**在于"你以为关了某个东西，其实没关"。

**处理**：查真实符号名，不要凭记忆：

```powershell
Select-String -Path managed_components\espressif__esp32-camera\Kconfig -Pattern '^\s+config \w+'
```

本版本已知的坑：

| 写了但不存在的键 | 正确的键 / 说明 |
|---|---|
| `CONFIG_SPIRAM_TYPE_OPI` | IDF 5.5 已删除；OPI 由 `CONFIG_SPIRAM_MODE_OCT` 单独表达 |
| `CONFIG_GC2035_SUPPORT` | 2.1.7 没有此型号 |
| `CONFIG_SC035HGS_SUPPORT` | 2.1.7 没有此型号 |

---

### `-Wformat-truncation` 把 snprintf 截断当 error

**根因**：IDF 在 `tools/cmake/build.cmake` 里对**所有组件**统一加 `-Wall -Werror`，
而且**没有**对应的 Kconfig 开关能单独放行 `format-truncation`。

**处理**：在 `main/CMakeLists.txt` 末尾只对本组件放行这一条：

```cmake
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format-truncation")
```

**不要**把 `-Werror` 整个关掉。

---

## 2. 传感器

### `camera init failed` / `err=0x105`（ESP_ERR_NOT_SUPPORTED / 探测不到）

按代价从低到高排查：

1. **`CAM_PIN_D0..D7` 顺序错** —— 这是最常见原因，
   D0..D7 必须严格对应丝印 Y2..Y9。
2. **SCCB 上拉缺失** —— 万用表量 SDA/SCL 对 3V3，应为 4~5 kΩ。
3. **XCLK 没出来** —— 用示波器看 `CAM_PIN_XCLK`（GPIO15），应有 20 MHz。
4. **供电不足** —— USB 线换后置口。
5. **摄像头排线插反/未插到底**。

---

### "代码按 OV5640 配 5MP，但运行起来 init failed"

**根因**：**忘了同步 `CONFIG_OV5640_SUPPORT`**。
代码分支（`CONFIG_CAM_OV5640`）改了，但固件里链的还是 OV2640 驱动。

**判据**：

```powershell
Select-String sdkconfig -Pattern '^CONFIG_CAM_OV\w+=y'        # 只能一行
Select-String sdkconfig -Pattern '^CONFIG_OV\w+_SUPPORT=y'    # 只能一行
```

两行必须是**同一个型号**。详见 [`sensors.md`](sensors.md)。

---

### 固件比预期大 40~60 KB

**根因**：`*_SUPPORT` 默认全 `y`，三个驱动都链进去了。
实测瘦身 **−19.5 KB**（1 321 936 → 1 302 448）。

**处理**：把不用的 `CONFIG_<型号>_SUPPORT` 全部显式写成 `is not set`。

---

## 3. SD 卡

| 现象 | 根因 | 处理 |
|---|---|---|
| 卡是好的但挂载失败 | 未格式化 FAT32 / 用了 exFAT | 格式化成 FAT32 |
| 卡在 `SD_MOUNTING` | 卡座接触不良 | 换卡排除 |
| 首次写成功、之后全失败 | 写保护 / 劣质卡 | 看 `sd_status_t.last_error` |
| `SD_LOWSPACE` 但空间没释放 | 卡里全是**未上传**文件 | **设计行为**，见 [`sd_modes.md`](sd_modes.md) §4 |
| 插卡后 Wi-Fi 变差/断连 | 供电不足 | 用后置 USB 口 |
| REQUIRED 模式无卡不启动 | 预期行为 | 插卡，或换 OPTIONAL 重编 |

---

## 4. 运行期

### `reset=BROWNOUT`

供电问题，先换 USB 线/口，不要先怀疑代码。
摄像头 + SD + Wi-Fi 同开瞬时电流 500 mA+。

### `reset=PANIC` / `TASK_WDT`

1. 看 panic 后面的 backtrace，`idf.py monitor` 会自动翻译地址到函数名；
2. 常见是**阻塞在 UI/网络回调里超时**；
3. 若和 framebuffer 有关，检查有没有漏 `esp_camera_fb_return()` 或 double free。

### 拍照成功但 `path=null`

**这不是错误**。说明 SD 不可用（无卡/降级），数据在 PSRAM 队列里，
`queued` 仍然递增。见 [`sd_modes.md`](sd_modes.md) §5。

### `409 {"error":"cooldown"}`

冷却生效，正常。等到 `min_trigger_interval_ms`（默认 3000 ms）过去，
或用 `?force=1` 绕过。

### `409 {"error":"disarmed"}`

未布防。先 `POST /api/arm`。

**注意 `?force=1` 绕不过这一条** —— 布防检查在 web 层就返回了。
force 只绕过冷却。（接口实测，别照"force 万能"的直觉写测试脚本。）

### 上传一直重试

```powershell
python tools/parse_log.py build_log_serial.txt
```

然后找 `[UPLOAD]`（HTTP）或 `[TG  ]`（Telegram）的行 ——
**没有 `UPLD` 这个 TAG**，日志 TAG 清单以 `main/logger.h` 为准。

看 `status=` 码：401/403 是密钥/URL 错，404 是路径错，超时是网络不通。
**注意**：重试不会阻塞拍照，队列会继续收新照片。

---

## 5. 构建脚本

### PowerShell 报 `Unexpected token` / 中文变乱码

**根因**：PowerShell 5.1 读 `.ps1` 时，**没有 BOM 就按 ANSI（GBK）解析**。
UTF-8 无 BOM 的中文注释会被解成乱码，进而撑坏语法。

**处理**：给脚本加 **UTF-8 BOM**：

```powershell
$c = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText($p, $c, (New-Object System.Text.UTF8Encoding($true)))
```

`tools/fix_bom.ps1` 已实现（本工程所有 `.ps1` 都已加 BOM）。
另外脚本的**输出文本已全部改成 ASCII**（`PASS` / `FAIL` / `RESULT:`），
避免中文在管道里被二次转码。

### `SDKCONFIG_DEFAULTS` 传多个文件没生效

**根因**：PowerShell 传 `-D` 时会把分号吃掉。

**处理**：用环境变量传（`build_matrix.ps1` 就是这么做的）：

```powershell
$env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;tools/cfg_overrides/sdkconfig.$name.defaults"
```

分号分隔是正确写法（IDF 的 `kconfig.cmake` 就按 `;` 拆）。

---

## 6. OTA / 清单服务器 / 端口 / 串口

这一块单独成篇：**上电升级只检查一次**、**8080 与 8081 的分工**、
**`request.host_url` 回环污染**、**Windows 允许重复绑定同一端口**、
**内嵌网页改完必须重新 build**、**COM 口被 `idf.py monitor` 占用**
—— 完整链路 + 逐条"现象 / 根因 / 处理 / 判据" + 可复制命令见
**[`debug_notes.md`](debug_notes.md)**。

最容易先踩的三条：

- **先起服务，再给设备上电**：`ota_check_on_boot()` 一辈子只在"上电那一刻"查一次，
  开机时 8081 不在就**永远不会**再升，日志只留一条 `ESP_ERR_HTTP_CONNECT`。
- **8081 = 升级清单，8080 = 收照片**：搞混就会看到
  `[OTA E] manifest 缺 "version" 字段 -> 无法判断，跳过`（那是被 `tools/recv_server.py` 抢答了）。
- **改完 `main/web/index.html` 必须重新 build + 烧录**：网页是编译期嵌进固件的。
