"""临时补丁：把 mqtt_start.bat 改成单窗口（broker 原地前台运行）。

原因：原脚本 start 出 broker 窗口 + 订阅窗口，加上本脚本自身共 3 个窗口。
broker 自己就会打印每条 PUBLISH，订阅窗口是重复的。
"""
import pathlib

p = pathlib.Path(r"d:\ESP32-IDF\esp-idf-v5.5.1\examples\get-started\esp32S3_CAM"
                 r"\rest_mock_server\mqtt_start.bat")
text = p.read_text(encoding="utf-8")
lines = text.split("\n")

# 定位 "rem ---------- 3. 起 broker" .. "endlocal" 之间
head = "rem ---------- 3."
start = next(i for i, l in enumerate(lines) if l.startswith(head))
end = next(i for i, l in enumerate(lines) if l.strip() == "endlocal")
print("replace lines", start + 1, "..", end + 1)
print("first:", lines[start])
print("last :", lines[end])

new_block = r"""rem ---------- 3. 起 broker（本窗口原地变成 broker，单窗口） ----------
rem  ★ 单窗口设计：不再 start 出第二个 broker 窗口、也不再开第三个订阅窗口。
rem    理由：broker 自己就会打印每条 PUBLISH（mqtt_broker.py 的 on_publish），
rem    再开一个订阅窗口纯属重复。要单独的订阅端就用 -w。
rem  ★ 本段刻意**不用括号代码块**：块内一旦有 start "标题" cmd /k "另一串引号"
rem    这种嵌套引号，cmd 会在解析整个块时就把引号配对搞错，随后 !PORT!
rem    展开位置错乱，报出莫名其妙的 "xxx was unexpected at this time"。
rem    改用 if + goto 的单行流，每行独立解析，引号不会跨行配对。
if not "!BROKER!"=="1" goto afterup

netstat -ano | findstr /R /C:":!PORT! .*LISTENING" >nul
if not errorlevel 1 goto broker_reuse

echo [信息] 启动 broker mqtt_broker.py 端口 !PORT! ...
echo.
echo --------------------------------------------
echo   设备配网页要填的 MQTT 参数:
echo --------------------------------------------
set "HASIP=0"
for /f "delims=" %%a in ('%PY% "%~dp0lan_ip.py" 2^>nul') do (
    echo   mqtt_uri   = mqtt://%%a:!PORT!
    echo   mqtt_user  = ^(留空^)    mqtt_pass = ^(留空^)
    set "HASIP=1"
)
if "!HASIP!"=="0" echo   mqtt_uri   = mqtt://^<你的局域网IP^>:!PORT!
echo.
echo   主题: esp32S3_CAM/^<MAC大写无分隔^>/event
echo   本板: esp32S3_CAM/288485922F44/event
echo   设备连不上时: 确认 mqtt_uri 里的 IP 是本机局域网 IP（不是 localhost）
echo --------------------------------------------
echo.
echo   下面就是 broker 本体，设备的连接/上报都打在这里。
echo   停止: 在本窗口按 Ctrl+C
echo.

rem broker 直接在本窗口前台跑，不 start 新窗口；关掉本窗口即停 broker。
rem 前台运行还能让 python 收到 Ctrl+C，走它自己的 KeyboardInterrupt 分支
rem 打印"累计收到 N 条 PUBLISH"，比 taskkill 干净。
"%PY%" "%~dp0mqtt_broker.py" --port !PORT!
goto ended

:broker_reuse
echo [提示] 端口 !PORT! 已在监听，复用现有 broker，不再另起。
echo        若那个 broker 是 mosquitto 且回 rc=5，就停掉它再来一次:
echo          netstat -ano ^| findstr :!PORT!    然后 taskkill /PID ^<pid^> /F
:afterup

rem ---------- 4. 只开订阅端（-w），或只打印信息（-b） ----------
if not "!WATCH!"=="1" goto no_watch

echo [信息] 以订阅端方式运行 mqtt_watch.py（本窗口）...
echo.
"%PY%" "%~dp0mqtt_watch.py" --host 127.0.0.1 --port !PORT!
goto ended

:no_watch
echo [信息] 端口 !PORT! 已有 broker 在跑，本脚本不再起任何东西。
echo        单独看消息:  mqtt_start.bat -w
echo.

:ended
echo.
echo   已退出。重开请再跑一次 mqtt_start.bat
echo.
pause
endlocal"""

lines[start:end + 1] = new_block.split("\n")

# 同步文件头注释：用法说明
head_map = {
    "rem  起本地 MQTT broker，并同时打开订阅窗口看设备上报。":
        "rem  起本地 MQTT broker（单窗口），设备的连接与上报都打在这个窗口。",
    "rem      mqtt_start.bat              起 broker + 订阅窗口":
        "rem      mqtt_start.bat              起 broker（本窗口原地变成 broker）",
    "rem      mqtt_start.bat -w           只开订阅窗口（broker 在别处跑）":
        "rem      mqtt_start.bat -w           只跑订阅端（broker 在别处跑）",
    "rem      mqtt_start.bat -b           只起 broker，不开订阅窗口":
        "rem      mqtt_start.bat -b           端口已被占用时只打印信息",
}
for i, l in enumerate(lines):
    if l in head_map:
        lines[i] = head_map[l]

# 标题也跟着改（原来写的是"Broker + 订阅查看"）
lines = [l.replace("title MQTT Broker + 订阅查看", "title MQTT Broker (单窗口)")
         for l in lines]

p.write_text("\r\n".join(lines), encoding="utf-8")
print("done, new line count", len(lines))
