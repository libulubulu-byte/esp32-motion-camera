# 临时脚本：停掉 PC 端的 MQTT broker（只针对监听 1883 的进程）
$pids = (Get-NetTCPConnection -LocalPort 1883 -State Listen -ErrorAction SilentlyContinue).OwningProcess |
    Select-Object -Unique
if (-not $pids) {
    Write-Output "no listener on 1883"
} else {
    foreach ($x in $pids) {
        $proc = Get-Process -Id $x -ErrorAction SilentlyContinue
        $name = if ($proc) { $proc.ProcessName } else { "unknown" }
        Stop-Process -Id $x -Force -ErrorAction SilentlyContinue
        Write-Output ("stopped pid=$x ($name)")
    }
}
Start-Sleep -Seconds 2
$left = netstat -ano | Select-String ":1883\s+.*LISTENING"
if ($left) { Write-Output ("STILL LISTENING: " + $left) } else { Write-Output "1883 free" }
