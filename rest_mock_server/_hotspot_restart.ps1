# 临时脚本：重启 Windows 移动热点（Stop -> Start），解决设备重连握手失败
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

function Await($op, $type) {
    $t = $asTaskGeneric.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait(30000) | Out-Null
    return $t.Result
}

[void][Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
[void][Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
$c = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
$p = $c::GetInternetConnectionProfile()
$tm = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($p)

Write-Output ("before : " + $tm.TetheringOperationalState + " clients=" + $tm.ClientCount)

$r = Await ($tm.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
Write-Output ("stop   : " + $r.Status + " " + $r.AdditionalErrorMessage)
Start-Sleep -Seconds 4

$r2 = Await ($tm.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
Write-Output ("start  : " + $r2.Status + " " + $r2.AdditionalErrorMessage)
Start-Sleep -Seconds 3

Write-Output ("after  : " + $tm.TetheringOperationalState + " clients=" + $tm.ClientCount)
Write-Output ("ssid   : " + $tm.GetCurrentAccessPointConfiguration().Ssid)
