# 临时脚本：把一张人脸图全屏显示在 PC 屏幕上，供 ESP32 相机拍摄
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$path = 'D:\ESP32-IDF\esp-idf-v5.5.1\examples\get-started\who_esp32S3_CAM\_inject\face.jpg'

$img = [System.Drawing.Image]::FromFile($path)
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.WindowState = 'Maximized'
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::Black

$pb = New-Object System.Windows.Forms.PictureBox
$pb.Dock = 'Fill'
$pb.SizeMode = 'Zoom'      # 撑满屏幕，尽量大
$pb.BackColor = [System.Drawing.Color]::Black
$pb.Image = $img
$form.Controls.Add($pb)

# 40 秒后自动关闭，避免一直挡屏幕
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 40000
$timer.Add_Tick({ $form.Close() })
$timer.Start()

[void]$form.ShowDialog()
$img.Dispose()
