# 临时脚本：启用 Telegram 中继所需的系统改动（hosts + 防火墙）
$log = Join-Path $env:TEMP 'relay_enable.log'
"=== $(Get-Date -Format 'HH:mm:ss') ===" | Out-File -FilePath $log -Encoding utf8

$hosts = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
try {
    $c = Get-Content -LiteralPath $hosts
    $c = $c | ForEach-Object { $_ -replace '^#\s*192\.168\.137\.1\s+api\.telegram\.org\s*$', '192.168.137.1   api.telegram.org' }
    if (-not ($c -match '^\s*192\.168\.137\.1\s+api\.telegram\.org')) {
        $c += '192.168.137.1   api.telegram.org'
    }
    Set-Content -LiteralPath $hosts -Value $c -Encoding ASCII
    Add-Content -FilePath $log -Value "hosts OK" -Encoding utf8
} catch {
    Add-Content -FilePath $log -Value ("hosts FAIL " + $_.Exception.Message) -Encoding utf8
}

foreach ($p in 443, 8080, 8081, 1883) {
    $n = "ESP32 relay port $p"
    try {
        if (-not (Get-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue)) {
            New-NetFirewallRule -DisplayName $n -Direction Inbound -Action Allow `
                -Protocol TCP -LocalPort $p -RemoteAddress 192.168.137.0/24 -Profile Any | Out-Null
            Add-Content -FilePath $log -Value "fw $p added" -Encoding utf8
        } else {
            Add-Content -FilePath $log -Value "fw $p exists" -Encoding utf8
        }
    } catch {
        Add-Content -FilePath $log -Value ("fw $p FAIL " + $_.Exception.Message) -Encoding utf8
    }
}

ipconfig /flushdns | Out-Null
Add-Content -FilePath $log -Value "RESULT: OK" -Encoding utf8
