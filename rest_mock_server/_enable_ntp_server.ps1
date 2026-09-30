# 临时脚本（需管理员）：把 Windows 时间服务切成 NTP 服务器，并放行热点子网的 UDP 123
# 目的：设备经 ICS 访问公网 NTP 的 UDP 转发不可靠，改由 PC 直接应答设备对时。
$log = Join-Path $env:TEMP 'ntp_server_setup.log'
"=== $(Get-Date -Format 'HH:mm:ss') ===" | Out-File -FilePath $log -Encoding utf8

try {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\NtpServer'
    New-ItemProperty -Path $key -Name 'Enabled' -PropertyType DWord -Value 1 -Force | Out-Null
    Add-Content $log "registry NtpServer Enabled=1 OK" -Encoding utf8
} catch { Add-Content $log ("registry FAIL " + $_.Exception.Message) -Encoding utf8 }

try {
    & w32tm /config /reliable:yes /update | Out-Null
    Add-Content $log "w32tm config reliable:yes OK" -Encoding utf8
} catch { Add-Content $log ("w32tm FAIL " + $_.Exception.Message) -Encoding utf8 }

try {
    Restart-Service w32time -Force
    Add-Content $log "w32time restarted OK" -Encoding utf8
} catch { Add-Content $log ("w32time restart FAIL " + $_.Exception.Message) -Encoding utf8 }

try {
    $n = 'ESP32 ntp udp123'
    if (-not (Get-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $n -Direction Inbound -Action Allow -Protocol UDP `
            -LocalPort 123 -RemoteAddress 192.168.137.0/24 -Profile Any | Out-Null
        Add-Content $log "firewall udp/123 added" -Encoding utf8
    } else {
        Add-Content $log "firewall udp/123 exists" -Encoding utf8
    }
} catch { Add-Content $log ("firewall FAIL " + $_.Exception.Message) -Encoding utf8 }

Add-Content $log "RESULT: OK" -Encoding utf8
