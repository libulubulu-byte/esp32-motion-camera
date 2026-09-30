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
rem  Must match the port used by start.bat / run_all.bat, otherwise the
rem  port-based fallback here sweeps a port nobody listens on and you get
rem  "service clearly still running, but stop says it found nothing".
setlocal enabledelayedexpansion
title REST Mock Server - stop

cd /d "%~dp0"

set "PORT=8081"
set "PIDFILE=%~dp0server.pid"

echo ============================================
echo   ESP32 REST mock backend - stop
echo ============================================
echo.

set "KILLED=0"

rem ---------- 1. kill by PID file ----------
if exist "%PIDFILE%" (
    set /p OLDPID=<"%PIDFILE%"
    tasklist /FI "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
    if not errorlevel 1 (
        taskkill /F /T /PID !OLDPID! >nul 2>nul
        if not errorlevel 1 (
            echo [OK] Stopped PID=!OLDPID!
            set "KILLED=1"
        ) else (
            echo [WARN] Failed to kill PID !OLDPID!, possibly insufficient rights.
        )
    ) else (
        echo [INFO] Process !OLDPID! from the pid file is already gone.
    )
    del "%PIDFILE%" >nul 2>nul
)

rem ---------- 2. fallback: sweep whoever holds the port ----------
for /f "tokens=5" %%p in ('netstat -ano ^| findstr /R /C:":%PORT% .*LISTENING"') do (
    if not "%%p"=="0" (
        taskkill /F /T /PID %%p >nul 2>nul
        if not errorlevel 1 (
            echo [OK] Port %PORT% was held by PID=%%p, terminated.
            set "KILLED=1"
        )
    )
)

echo.
if "!KILLED!"=="1" (
    echo Service stopped. History is kept in telemetry.db and keeps accumulating.
) else (
    echo No running service found.
)
echo.
pause
endlocal
