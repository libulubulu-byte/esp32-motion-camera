@echo off
rem ==========================================================================
rem  NOTE FOR EDITORS -- keep this file PURE ASCII.
rem
rem  cmd.exe parses .bat files through the *system ANSI codepage* (cp936 on
rem  this box), not the codepage "chcp 65001" sets at runtime. UTF-8 Chinese
rem  comments are 3 bytes/char; read through cp936 they desync the parser, so
rem  "goto" lands on the wrong line, ":" labels stop matching, and the tail of
rem  a comment gets executed as a command. Chinese belongs in the .py files.
rem ==========================================================================
rem
rem  Port 8081 (changed 2026-09-27, was 8080): 8080 is reserved for
rem  tools/recv_server.py which receives photos; this service owns /ota.json
rem  and /firmware/. It must match the APP_OTA_CHECK_URL port in
rem  main\app_conf.h, otherwise the device cannot fetch the manifest.
setlocal enabledelayedexpansion
title REST Mock Server - start

cd /d "%~dp0"

set "PORT=8081"
set "PIDFILE=%~dp0server.pid"
set "LOGFILE=%~dp0server.log"

echo ============================================
echo   ESP32 REST mock backend - start
echo ============================================
echo.

rem ---------- 1. locate Python ----------
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERROR] Python not found.
    echo         Install Python 3 ^(https://www.python.org/downloads/^)
    echo         and tick "Add python.exe to PATH".
    echo.
    pause
    exit /b 1
)

rem ---------- 2. already running: just exit ----------
if exist "%PIDFILE%" (
    set /p OLDPID=<"%PIDFILE%"
    tasklist /FI "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
    if not errorlevel 1 (
        echo [INFO] Service already running ^(PID=!OLDPID!^).
        echo        Open http://localhost:%PORT%/ in a browser.
        echo        Run stop.bat first if you want to restart it.
        echo.
        pause
        exit /b 0
    )
    del "%PIDFILE%" >nul 2>nul
)

rem ---------- 3. check the port ----------
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 (
    echo [ERROR] Port %PORT% already in use, possibly by another program or a
    echo         previous run that did not exit cleanly.
    echo         Find the owner:  netstat -ano ^| findstr :%PORT%
    echo         Kill it:         taskkill /F /PID ^<PID-from-above^>
    echo         Or change the port in this file: set "PORT=8081"
    echo.
    pause
    exit /b 1
)

rem ---------- 4. check Flask ----------
%PY% -c "import flask" >nul 2>nul
if errorlevel 1 (
    echo [INFO] Flask missing, installing automatically...
    %PY% -m pip install --quiet --disable-pip-version-check flask
    if errorlevel 1 (
        echo [ERROR] Flask install failed. Try manually:
        echo         %PY% -m pip install flask
        echo.
        pause
        exit /b 1
    )
    echo [INFO] Flask installed.
)

rem ---------- 5. find the LAN IP for the device config page ----------
echo --------------------------------------------
echo   REST settings to enter on the device web page:
echo --------------------------------------------
set "HASIP=0"
for /f "delims=" %%a in ('%PY% "%~dp0lan_ip.py" 2^>nul') do (
    echo   rest_host  = %%a
    set "HASIP=1"
)
if "!HASIP!"=="0" (
    echo   rest_host  = ^<no LAN IP found, check ipconfig yourself^>
)
echo   rest_port  = %PORT%
echo   rest_path  = /report
echo   rest_cmd   = /lampcmd
echo   rest_tls   = off
echo.
echo   Web console:  http://localhost:%PORT%/
echo --------------------------------------------
echo.

rem ---------- 6. launch in the background ----------
echo [INFO] Starting service...
%PY% "%~dp0launch.py" >nul 2>nul

if not exist "%PIDFILE%" (
    echo [ERROR] Launch failed, see log: %LOGFILE%
    echo.
    pause
    exit /b 1
)

rem Wait for it to listen
set /p NEWPID=<"%PIDFILE%"
set /a WAITCNT=0
:waitloop
rem Use ping for a 1s delay: timeout errors out and spins when stdin is redirected
ping -n 2 127.0.0.1 >nul
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 goto running
set /a WAITCNT+=1
if !WAITCNT! lss 15 goto waitloop

echo [WARN] Port not up after 15s; the process is probably erroring out.
echo        Log contents:
echo --------------------------------------------
type "%LOGFILE%"
echo --------------------------------------------
echo.
pause
exit /b 1

:running
echo [OK] Service started ^(PID=!NEWPID!^), listening on 0.0.0.0:%PORT%
echo.
echo   Stop service: double-click stop.bat
echo   View log:     %LOGFILE%
echo.

rem ---------- 7. open the browser ----------
start "" "http://localhost:%PORT%/"

echo Press any key to close this window (the service keeps running).
pause >nul
endlocal
