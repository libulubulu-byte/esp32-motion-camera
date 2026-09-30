<#
  telegram_relay.py 的配套设置脚本（需管理员）。

  做两件事：
    1) hosts 增加  192.168.137.1  api.telegram.org
       —— ICS 的 DNS 会走 PC 的解析器，于是热点上的设备把域名解析到 PC，连到中继；
          中继再经 PC 的代理连真正的 Telegram（TLS 仍是设备↔Telegram 端到端）。
    2) 防火墙放行 192.168.137.0/24 -> 本机 TCP 443

  用法：
    powershell -ExecutionPolicy Bypass -File relay_setup.ps1          # 安装
    powershell -ExecutionPolicy Bypass -File relay_setup.ps1 -Remove  # 撤销

  背景与判据见 ../docs/debug_notes.md §9.7。
#>
param([switch]$Remove)

$log = Join-Path $env:TEMP 'relay_setup.log'
function W($m) { $m | Out-File -FilePath $log -Append -Encoding utf8 }

$hostsPath = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$marker    = '# ESP32 telegram relay'
$line      = '192.168.137.1   api.telegram.org'
$ruleName  = 'ESP32 telegram relay'

"=== $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  Remove=$Remove ===" |
    Out-File -FilePath $log -Encoding utf8

try {
    if ($Remove) {
        $h = Get-Content -LiteralPath $hostsPath -Encoding UTF8
        $h = $h | Where-Object { $_ -notlike "*$marker*" -and $_ -notmatch '^\s*192\.168\.137\.1\s+api\.telegram\.org' }
        Set-Content -LiteralPath $hostsPath -Value $h -Encoding ASCII
        W "hosts 行已移除"

        if (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue) {
            Remove-NetFirewallRule -DisplayName $ruleName
            W "防火墙规则已删除"
        }
    } else {
        $h = Get-Content -LiteralPath $hostsPath -Encoding UTF8
        if ($h -notmatch 'api\.telegram\.org') {
            Add-Content -LiteralPath $hostsPath -Value "$marker`r`n$line" -Encoding ASCII
            W "hosts 已追加: $line"
        } else {
            W "hosts 里已有 api.telegram.org，跳过"
        }

        if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
            New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow `
                -Protocol TCP -LocalPort 443 -RemoteAddress 192.168.137.0/24 -Profile Any | Out-Null
            W "防火墙规则已添加 (TCP 443 from 192.168.137.0/24)"
        } else {
            W "防火墙规则已存在"
        }
    }

    ipconfig /flushdns | Out-Null
    W "已 flushdns"
    W "RESULT: OK"
} catch {
    W ("RESULT: ERROR " + $_.Exception.Message)
}
