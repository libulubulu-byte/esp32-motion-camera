# tools/build.ps1 —— 编译并把完整日志存 build_log_<name>.txt，只回显关键行
# 用法：  powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 [配置名]
#
# 注意：产物名是 build\esp32s3_snapshot_kit.bin（工程名来自根 CMakeLists.txt 的
# project()），不是目录名 esp32S3_CAM。
param([string]$Name = "default")

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null
Set-Location $root

$log = Join-Path $root "build_log_$Name.txt"
idf.py build *>&1 | Tee-Object -FilePath $log | Out-Null

$bin = Join-Path $root "build\esp32s3_snapshot_kit.bin"
$errs = Select-String -Path $log -Pattern 'error: ', 'FAILED:', 'fatal error', 'undefined reference'
$didComplete = Select-String -Path $log -Pattern 'Project build complete' -ErrorAction SilentlyContinue

if ($errs) {
    Write-Output "=========== 错误行 ($($errs.Count)) ==========="
    $errs | Select-Object -First 60 | ForEach-Object { Write-Output $_.Line.Trim() }
}
if ($didComplete) {
    Write-Output "=========== BUILD OK ==========="
} else {
    Write-Output "=========== BUILD FAILED ==========="
    Get-Content $log -Tail 25 | ForEach-Object { Write-Output "| $_" }
}

$w = Select-String -Path $log -Pattern 'warning: '
if ($w) { Write-Output ("警告 {0} 条，完整日志: {1}" -f $w.Count, $log) }
if (Test-Path $bin) {
    Write-Output ("BIN: {0} bytes" -f (Get-Item $bin).Length)
}
