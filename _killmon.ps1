# 临时脚本：停掉后台串口监听（_serialmon.py）释放 COM9
$procs = Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Where-Object { $_.CommandLine -like '*_serialmon*' }
if (-not $procs) {
    Write-Output "no monitor process found"
} else {
    foreach ($p in $procs) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        Write-Output ("killed monitor pid=" + $p.ProcessId)
    }
}
Start-Sleep -Seconds 1
