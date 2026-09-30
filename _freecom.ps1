# 临时脚本：结束占用 COM9 的残留 monitor 进程（保留 relay / 8080 / 8081 服务）
$targets = Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Where-Object { $_.CommandLine -like '*COM9*' -and $_.CommandLine -notlike '*_relay_dbg*' -and
                   $_.CommandLine -notlike '*recv_server*' -and $_.CommandLine -notlike '*server.py*' }
if (-not $targets) {
    Write-Output "no COM9 holder found"
} else {
    foreach ($p in $targets) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        Write-Output ("killed pid=" + $p.ProcessId)
    }
}
Start-Sleep -Seconds 2
# 复核：COM9 是否已释放
$left = Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Where-Object { $_.CommandLine -like '*COM9*' }
if ($left) { Write-Output ("STILL HELD by: " + (($left | ForEach-Object { $_.ProcessId }) -join ',')) }
else { Write-Output "COM9 free" }
