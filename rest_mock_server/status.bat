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
rem  Keep in sync with start.bat / run_all.bat / stop.bat
rem  (changed 2026-09-27 from 8080 to 8081).
rem  Checking the wrong port shows "nothing listening" and looks like a dead
rem  service.
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "PORT=8081"
set "PIDFILE=%~dp0server.pid"

echo ============================================
echo   ESP32 REST mock backend - status
echo ============================================
echo.

rem ---------- process status ----------
if exist "%PIDFILE%" (
    set /p PID=<"%PIDFILE%"
    tasklist /FI "PID eq !PID!" 2>nul | find "!PID!" >nul
    if not errorlevel 1 (
        echo [RUNNING] PID=!PID!
    ) else (
        echo [STOPPED] PID file exists but process !PID! is gone; run stop.bat to clean up.
    )
) else (
    echo [STOPPED] No PID file.
)

rem ---------- port status ----------
echo.
echo Listeners on port %PORT%:
netstat -ano | findstr /R /C:":%PORT% .*LISTENING"
if errorlevel 1 echo   ^(nothing is listening on %PORT%^)

rem ---------- HTTP liveness ----------
echo.
echo HTTP probe http://localhost:%PORT%/api/stats :
curl -s -m 3 http://localhost:%PORT%/api/stats
if errorlevel 1 (
    echo   Request failed; the service is probably not up.
) else (
    echo.
)

echo.
echo Log file: %~dp0server.log
echo.
pause
endlocal
