# 临时脚本：列出 python 进程及其命令行（用于找出谁占着 COM9）
Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Select-Object ProcessId, CommandLine |
    Format-List |
    Out-String -Width 300
