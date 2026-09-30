@echo off
rem ===========================================================================
rem  NOTE FOR EDITORS -- READ BEFORE TOUCHING THIS FILE
rem
rem  This .bat must stay PURE ASCII (7-bit) on a GBK/936 system.
rem
rem  cmd.exe parses .bat files using the *system ANSI codepage* (cp936 here),
rem  NOT the codepage that "chcp 65001" sets at runtime. A UTF-8 Chinese
rem  comment is 3 bytes/char, but cmd reads those bytes through cp936, and
rem  whenever a byte sequence fails to decode the parser loses byte alignment.
rem  The damage is severe and misleading:
rem      - "goto <label>" lands on the wrong line
rem      - ":" labels stop being recognised
rem      - the tail of a comment gets executed as a command, producing
rem        "'<mangled text>' is not recognized as an internal or external command"
rem  So: keep every comment and every echoed string in this file ASCII-only.
rem  Chinese user-facing text belongs in the Python side (mqtt_broker.py /
rem  mqtt_watch.py), which is read as UTF-8 and is unaffected.
rem ===========================================================================
rem
rem  Starts a local MQTT broker in THIS window; device connect/publish logs
rem  appear here.
rem
rem  The broker is mqtt_broker.py -- pure Python, zero deps, no install,
rem  no admin. Why not mosquitto: the official Windows package is an NSIS
rem  installer carrying a requireAdministrator manifest, so even a silent
rem  install trips WinError 740; and this box has no Node, so the
rem  "npx aedes" route is out too. The device only publishes, so the
rem  requirement is tiny and a hand-rolled broker is less trouble.
rem
rem  Single-window design: the broker runs in the foreground of THIS window
rem  (no "start" of a second window), so the script opens exactly one window.
rem  The broker itself prints every PUBLISH, so no extra subscriber window is
rem  needed; use -w if you want a standalone subscriber.
rem
rem  Device side (main/app_conf.h) should be filled in as:
rem      APP_MQTT_HOST         = <this PC LAN IP>
rem      APP_MQTT_PORT         = 1883
rem      APP_MQTT_TOPIC_PREFIX = esp32S3_CAM
rem
rem  Usage:
rem      mqtt_start.bat              start broker (this window becomes it)
rem      mqtt_start.bat -w           subscriber only (broker runs elsewhere)
rem      mqtt_start.bat -b           port already in use: print info only
rem      mqtt_start.bat -port 1884   use another port
rem ===========================================================================

rem ---------------------------------------------------------------------------
rem  setlocal enabledelayedexpansion is REQUIRED, not optional.
rem  This script reads variables with !VAR! (delayed expansion) in several
rem  places: the !WATCH! / !BROKER! option branches, the ":!PORT!" findstr
rem  pattern, the "%PY% ... for /f" block that sets HASIP, and every echo.
rem  Without it, !VAR! is NOT expanded at all -- cmd compares the literal
rem  three-character text "!WATCH!" against "1", which never matches, so the
rem  script silently falls through to the :no_watch branch and prints
rem  "Port !PORT! already has a broker" without ever starting anything.
rem  Observed for real on 2026-09-28 (the broker had to be started by hand).
rem  The matching endlocal already exists at the end of this file.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "PORT=1883"
set "WATCH=1"
set "BROKER=1"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-w"    goto opt_w
if /i "%~1"=="-b"    goto opt_b
if /i "%~1"=="-port" goto opt_port
if /i "%~1"=="-h"     goto usage
if /i "%~1"=="-help"  goto usage
if /i "%~1"=="--help" goto usage
echo [ERROR] Unknown argument: %~1
goto usage

rem Use goto instead of an inline ( ... ^& shift ^& goto ... ) chain:
rem a ^& inside parentheses makes cmd split the whole line at parse time,
rem which eats half the quotes in set "PORT=%~2", and the later !PORT!
rem expansion then fails with "unexpected at this time".
:opt_w
set "BROKER=0"
shift
goto parse
:opt_b
set "WATCH=0"
shift
goto parse
:opt_port
set "PORT=%~2"
shift
shift
goto parse
:parsed

echo ============================================
echo   ESP32 Snapshot Kit - MQTT environment
echo ============================================
echo.

rem ---------- 1. locate Python ----------
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERROR] Python not found. Both broker and subscriber are Python scripts.
    echo         Install: https://www.python.org/downloads/
    echo         Tick "Add python.exe to PATH" during setup.
    echo.
    pause
    exit /b 1
)
echo [INFO] Python: %PY%

rem ---------- 2. subscriber needs paho-mqtt (broker itself has zero deps) ----------
rem  Only -w (subscriber-only) needs paho; starting a broker does not, so we
rem  avoid a pointless install in that case.
if not "!WATCH!"=="1" goto need_paho_done
if "!BROKER!"=="1" goto need_paho_done

%PY% -c "import paho.mqtt.client" >nul 2>nul
if not errorlevel 1 goto need_paho_done

echo [INFO] paho-mqtt missing, installing automatically...
%PY% -m pip install --quiet --disable-pip-version-check paho-mqtt
if errorlevel 1 (
    echo [ERROR] paho-mqtt install failed. Install it manually:
    echo         %PY% -m pip install paho-mqtt
    echo.
    pause
    exit /b 1
)
echo [INFO] paho-mqtt installed.
:need_paho_done

rem ---------- 3. start broker (this window becomes the broker) ----------
rem  Single-window design: never "start" a second broker window, and never a
rem  third subscriber window. The broker already prints every PUBLISH (see
rem  on_publish in mqtt_broker.py), so a second subscriber would just duplicate.
rem  Use -w when you want a separate subscriber.
rem  This section deliberately avoids parenthesised blocks: once a block holds
rem  a nested-quote construct like  start "title" cmd /k "another string" ,
rem  cmd mis-pairs the quotes while parsing the whole block, then !PORT! expands
rem  in the wrong place and you get a nonsensical "unexpected at this time".
rem  A flat if + goto flow parses one line at a time, so quotes never pair
rem  across lines.
if not "!BROKER!"=="1" goto afterup

netstat -ano | findstr /R /C:":!PORT! .*LISTENING" >nul
if not errorlevel 1 goto broker_reuse

echo [INFO] Starting broker mqtt_broker.py on port !PORT! ...
echo.
echo --------------------------------------------
echo   MQTT settings to enter on the device web page:
echo --------------------------------------------
set "HASIP=0"
for /f "delims=" %%a in ('%PY% "%~dp0lan_ip.py" 2^>nul') do (
    echo   mqtt_uri   = mqtt://%%a:!PORT!
    rem  Parentheses inside a "for ... do ( ... )" block MUST be caret-escaped.
    rem  A bare "(" starts a nested block, so cmd fails to parse the rest of the
    rem  line and aborts the WHOLE script with
    rem      mqtt_pass was unexpected at this time.
    rem  and the broker never starts. Escaping as ^( ^) keeps the literal text.
    echo   mqtt_user  = ^(blank^)    mqtt_pass = ^(blank^)
    set "HASIP=1"
)
if "!HASIP!"=="0" echo   mqtt_uri   = mqtt://^<your-LAN-IP^>:!PORT!
echo.
echo   Topic: esp32S3_CAM/^<MAC-uppercase-no-separator^>/event
echo   This board: esp32S3_CAM/288485922F44/event
echo   If the device cannot connect: make sure the IP in mqtt_uri is this
echo   PC's LAN IP, not localhost.
echo --------------------------------------------
echo.
echo   The broker is starting below; device connect/publish logs show here.
echo   Stop: press Ctrl+C in this window
echo.

rem The broker runs in the foreground of this window, no "start"; closing the
rem window stops it. Foreground also lets Python receive Ctrl+C, so it takes
rem its own KeyboardInterrupt path and prints the total PUBLISH count, which
rem is cleaner than taskkill.
"%PY%" "%~dp0mqtt_broker.py" --port !PORT!
goto ended

:broker_reuse
echo [WARN] Port !PORT! is already listening; reusing the existing broker.
echo        If this repo's broker is NOT the one running, the listener is
echo        probably mosquitto. It defaults to allow_anonymous false with no
echo        password_file, so the subscriber will spin failing to reconnect
echo        with rc=135 (peer closed during handshake -- not the rc=5 that
echo        usually means auth failure).
echo        First identify the owner:
echo          netstat -ano ^| findstr :!PORT!      last column is the PID
echo          tasklist /FI "PID eq ^<pid^>"        shows the process name
echo        If it is mosquitto.exe, stop it and retry:
echo          taskkill /PID ^<pid^> /F             then rerun mqtt_start.bat
:afterup

rem ---------- 4. subscriber only (-w), or info only (-b) ----------
if not "!WATCH!"=="1" goto no_watch

echo [INFO] Running mqtt_watch.py as a subscriber (this window)...
echo.
"%PY%" "%~dp0mqtt_watch.py" --host 127.0.0.1 --port !PORT!
goto ended

:no_watch
echo [INFO] Port !PORT! already has a broker; this script starts nothing.
echo        Watch messages separately:  mqtt_start.bat -w
echo.

:ended
echo.
echo   Exited. Run mqtt_start.bat again to restart.
echo.
pause
endlocal
exit /b 0

:usage
echo.
echo Usage:
echo     mqtt_start.bat              start broker (single window)
echo     mqtt_start.bat -w           subscriber only
echo     mqtt_start.bat -b           info only when port is taken
echo     mqtt_start.bat -port 1884   use another port
echo.
pause
exit /b 0
