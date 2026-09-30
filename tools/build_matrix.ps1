# tools/build_matrix.ps1 —— 任务书第 8 节的 6 组编译矩阵
#
# 每组做法：fullclean -> 用 SDKCONFIG_DEFAULTS **环境变量** 叠加组专属片段 -> build
# 日志存 build_log_<cfg>.txt。
#
# ★ 为什么用环境变量而不是 `idf.py -D SDKCONFIG_DEFAULTS=a;b build`：
#   PowerShell 会把未加引号的 `;` 当语句分隔符，加引号又会把引号一起传给
#   python 的 argparse（实测报 "invalid value ... 'a;b'" 或直接静默只用第一个文件）。
#   设成进程环境变量则完全绕开命令行解析，这是 IDF 官方测试里也在用的方式
#   （tools/test_build_system/test_non_default_target.py:190）。
#
# 用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_matrix.ps1
#        powershell ... -File tools/build_matrix.ps1 -Only ov5640_optional

param([string]$Only = "")

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null
Set-Location $root

$overrides = Join-Path $root "tools\cfg_overrides"
New-Item -ItemType Directory -Force -Path $overrides | Out-Null

# ---- 各组专属片段（叠加在根 sdkconfig.defaults 之后；后者覆盖前者） ----
# 只写"与默认不同"的键。注意把 kconfig 的 "=n" 写成 "= " 空值或 "# ... is not set"
# 都行；这里统一用 `# CONFIG_X is not set`，这是 kconfig 自己写回来的标准形式。

# ★★ 2026-09-27：根 sdkconfig.defaults 的默认传感器已从 OV5640 改为 **OV3660**
#   （真机实测板子插的是 PID=0x3660）。因此下面**每一组都必须显式声明传感器**，
#   不能再有"靠继承根 defaults"的组 —— 否则组名与产物对不上，
#   例如名字叫 ov2640_* 的组实际编出来是 OV3660，排查时会被自己误导。
#   同时组名也统一改成按"传感器_其他维度"命名。

$cfg_ov2640 = @"
# === 覆盖：OV2640（2MP） ===
CONFIG_CAM_OV3660=
CONFIG_CAM_OV2640=y
CONFIG_CAM_OV5640=
CONFIG_OV3660_SUPPORT=
CONFIG_OV2640_SUPPORT=y
CONFIG_OV5640_SUPPORT=
"@

$cfg_ov3660 = @"
# === 覆盖：OV3660（3MP，KPEG 支持 QXGA）—— 本机实机型号 ===
CONFIG_CAM_OV2640=
CONFIG_CAM_OV3660=y
CONFIG_CAM_OV5640=
CONFIG_OV2640_SUPPORT=
CONFIG_OV3660_SUPPORT=y
CONFIG_OV5640_SUPPORT=
"@

$cfg_ov5640 = @"
# === 覆盖：OV5640（5MP，PSRAM 压力最大） ===
CONFIG_CAM_OV2640=
CONFIG_CAM_OV3660=
CONFIG_CAM_OV5640=y
CONFIG_OV2640_SUPPORT=
CONFIG_OV3660_SUPPORT=
CONFIG_OV5640_SUPPORT=y
"@

$cfg_sd_none = @"
# === 覆盖：SD 三态 = NONE（完全不编 SD，验证"无卡必须能启动"） ===
$cfg_ov2640
CONFIG_SD_MODE_NONE=y
CONFIG_SD_MODE_OPTIONAL=
CONFIG_SD_MODE_REQUIRED=
"@

$cfg_sd_required = @"
# === 覆盖：SD 三态 = REQUIRED（无卡即红灯） ===
$cfg_ov2640
CONFIG_SD_MODE_NONE=
CONFIG_SD_MODE_OPTIONAL=
CONFIG_SD_MODE_REQUIRED=y
"@

$cfg_full = @"
# === 覆盖：全功能（MQTT + MJPEG stream + human stub，传感器用 OV5640） ===
CONFIG_CAM_OV2640=
CONFIG_CAM_OV3660=
CONFIG_CAM_OV5640=y
CONFIG_OV2640_SUPPORT=
CONFIG_OV3660_SUPPORT=
CONFIG_OV5640_SUPPORT=y
CONFIG_SNAP_ENABLE_MQTT=y
CONFIG_SNAP_ENABLE_STREAM=y
CONFIG_SNAP_ENABLE_HUMAN_STUB=y
"@

$matrix = @(
    @{ Name = "ov3660_optional";    Body = $cfg_ov3660 },   # ← 本机实机配置，放第一组
    @{ Name = "ov2640_optional";    Body = $cfg_ov2640 },
    @{ Name = "ov5640_optional";    Body = $cfg_ov5640 },
    @{ Name = "ov2640_sd_none";     Body = $cfg_sd_none },
    @{ Name = "ov2640_sd_required"; Body = $cfg_sd_required },
    @{ Name = "full_features";      Body = $cfg_full }
)

# 让 PowerShell 的输出编码不与 python 的 stdout 打架
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

# ★ 必须把 python 的 stdio 也钉成 UTF-8。
#   本机 python 3.13，默认 stdout/stderr 编码是 GBK（cp936）。cmake 在打印
#   "Loading defaults file ..." 时会把某个非 GBK 字符送出来，idf.py 的
#   read_and_write_stream() 直接 output_converter.write() 崩溃：
#       UnicodeEncodeError: 'gbk' codec can't encode character '\ufffd'
#   注意：**每一组配置前都要重设一次** —— idf.py fullclean 之后环境变量有可能
#   被 subprocess 继承链重置，而且 PowerShell 的 [Environment]::SetEnvironmentVariable
#   在只传两个参数时是"进程级"，跨 idf.py 调用是可靠的，但为稳妥起见放在循环里。
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
$env:PYTHONLEGACYWINDOWSSTDIO = "0"

$summary = @()

foreach ($m in $matrix) {
    if ($Only -and $m.Name -ne $Only) { continue }
    Write-Output ""
    Write-Output "#################### [$($m.Name)] ####################"

    # 组专属片段（无 BOM！kconfig 会把 BOM 当成第一个键的一部分）
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    if ($m.Body -eq "") {
        $env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults"
    } else {
        $frag = Join-Path $overrides "sdkconfig.$($m.Name).defaults"
        [System.IO.File]::WriteAllText($frag, $m.Body, $utf8)
        $env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;tools/cfg_overrides/sdkconfig.$($m.Name).defaults"
    }

    # 每组都重设一次（见文件头的说明）
    $env:PYTHONIOENCODING = "utf-8"
    $env:PYTHONUTF8 = "1"

    # fullclean 顺带把上一组的 sdkconfig 删掉，保证这一组从 defaults 重新生成
    idf.py fullclean *>&1 | Out-Null
    if (Test-Path (Join-Path $root "sdkconfig")) { Remove-Item (Join-Path $root "sdkconfig") -Force }

    $log = Join-Path $root "build_log_$($m.Name).txt"
    idf.py build *>&1 | Tee-Object -FilePath $log | Out-Null

    # ★ 工程名是 esp32s3_snapshot_kit（见根 CMakeLists.txt 的 project()），
    #   产物是 build\esp32s3_snapshot_kit.bin，不是目录名 esp32S3_CAM.bin。
    $bin = Join-Path $root "build\esp32s3_snapshot_kit.bin"
    $size = if (Test-Path $bin) { (Get-Item $bin).Length } else { 0 }
    # 判据以"产物存在 + 日志里出现成功标志"为准，不靠 grep error 字样
    # （源文件里的中文字符串会被当 GBK 解码成乱码，让 UnicodeEncodeError 之类
    #   的字面量误命中；IDF 自身也会打印 "0 errors" 这类文本）。
    $okFlagF = Select-String -Path $log -Pattern 'Project build complete' -ErrorAction SilentlyContinue
    $okFlagG = Select-String -Path $log -Pattern 'Successfully created esp32s3 image' -ErrorAction SilentlyContinue
    $ok = (Test-Path $bin) -and $okFlagF -and $okFlagG

    if ($ok) {
        Write-Output "  [PASS] $($m.Name)  bin=$size bytes"
    } else {
        Write-Output "  [FAIL] $($m.Name)  bin=$size bytes"
        Select-String -Path $log -Pattern 'error: ', 'FAILED:', 'fatal error', 'undefined reference' |
            Select-Object -First 20 | ForEach-Object { Write-Output "    $($_.Line.Trim())" }
        Get-Content $log -Tail 15 | ForEach-Object { Write-Output "    | $_" }
    }

    # 记录这一组生成的关键配置，便于核对"到底编成了什么"
    $cfgFile = Join-Path $root "sdkconfig"
    $get = { param($p) $h = Select-String -Path $cfgFile -Pattern $p | Select-Object -First 1
             if ($h) { ($h.Line -split '=')[0].Trim() } else { "-" } }
    $cam  = & $get '^CONFIG_CAM_OV\w+=y'
    $sup  = & $get '^CONFIG_OV\w+_SUPPORT=y'
    $sd   = & $get '^CONFIG_SD_MODE_\w+=y'
    $mqtt = & $get '^CONFIG_SNAP_ENABLE_MQTT=y'
    $strm = & $get '^CONFIG_SNAP_ENABLE_STREAM=y'
    Write-Output "  VERIFY: $cam | $sup | $sd | mqtt=$mqtt | stream=$strm"
    $mark = if ($ok) { "PASS" } else { "FAIL" }
    $summary += [pscustomobject]@{ Config = $m.Name; Result = $mark;
                                   BinBytes = $size; Sensor = $cam; Support = $sup;
                                   Sd = $sd; Mqtt = $mqtt; Stream = $strm }
}

Remove-Item Env:\SDKCONFIG_DEFAULTS -ErrorAction SilentlyContinue

Write-Output ""
Write-Output "================ BUILD MATRIX SUMMARY ================"
$summary | Format-Table -AutoSize | Out-String -Width 200 | Write-Output

$failed = @($summary | Where-Object { $_.Result -eq "FAIL" }).Count
$passed = @($summary | Where-Object { $_.Result -eq "PASS" }).Count
$total = @($summary).Count
Write-Output "RESULT: $passed / $total configurations built OK"
if ($failed -gt 0) { exit 1 }
