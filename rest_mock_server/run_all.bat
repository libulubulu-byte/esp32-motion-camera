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
rem  This script does four things, and the order matters:
rem    1. Build firmware (idf.py build) -- produces a fresh .bin whose image
rem       header carries the version from version.h
rem    2. Publish to firmware\  -- server.py reads /ota.json from here
rem    3. Start server.py      -- serves /ota.json, /firmware/*, and the dashboard
rem    4. Print the IP the device should use, then self-check
rem
rem  Why step 2 is mandatory: editing version.h and only building leaves the old
rem  .bin in firmware\, so /ota.json still reports the old version and the
rem  device concludes it is already up to date.
rem ==========================================================================

cd /d "%~dp0"
set "HERE=%~dp0"
set "PROJ=%HERE%.."
rem Port 8081, not 8080 (changed 2026-09-27):
rem   8080 is often taken by tools/recv_server.py (the minimal photo receiver),
rem   and that one answers EVERY GET path with {"server":"alive",...}. If the
rem   device reads that as /ota.json it logs
rem       [OTA E] manifest missing "version" field -> cannot judge, skip
rem   and OTA looks broken. Separate ports keep them independent: photo upload
rem   on 8080, update manifest on 8081.
rem   This value must match the port in APP_OTA_CHECK_URL in main\app_conf.h.
set "PORT=8081"
set "PIDFILE=%HERE%server.pid"
set "LOGFILE=%HERE%server.log"

rem Switch to skip the build: run_all.bat -nobuild  (restart service only)
set "SKIP_BUILD=0"
if /i "%~1"=="-nobuild" set "SKIP_BUILD=1"
if /i "%~1"=="--no-build" set "SKIP_BUILD=1"

echo ============================================
echo   esp32S3_CAM one-click start
echo ============================================
echo.

rem ---------- 0. locate Python ----------
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERROR] Python not found. Install it and tick "Add python.exe to PATH".
    echo         https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
)

rem ---------- 1. stop the old service so the port is free ----------
rem  Kill by PID first, then sweep by port as a fallback. PID alone is not
rem  enough: if server.pid does not match the real listener (e.g. last time it
rem  was started by hand with "python server.py", or the pid file is stale),
rem  killing that PID does nothing and we later hit "port is already in use".
rem  stop.bat uses the same belt-and-braces approach.
if exist "%PIDFILE%" (
    set /p OLDPID=<"%PIDFILE%"
    tasklist /FI "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
    if not errorlevel 1 (
        echo [INFO] Stopping old service PID=!OLDPID! ...
        taskkill /F /T /PID !OLDPID! >nul 2>nul
    )
    del "%PIDFILE%" >nul 2>nul
)

rem Fallback: whoever listens on the port gets terminated
for /f "tokens=5" %%p in ('netstat -ano ^| findstr /R /C:":%PORT% .*LISTENING"') do (
    if not "%%p"=="0" (
        echo [INFO] Port %PORT% held by PID=%%p, terminating to release it...
        taskkill /F /T /PID %%p >nul 2>nul
    )
)

rem Give the OS a moment to actually release the port (process exit is async)
ping -n 2 127.0.0.1 >nul

rem ---------- 2. build firmware ----------
if "!SKIP_BUILD!"=="1" (
    echo [SKIP] Not rebuilding firmware, as requested.
    echo.
) else (
    rem Show the current version so it is obvious what this build produces.
    rem
    rem Parsing is delegated to check_bin_version.py --header-only instead of
    rem scraping strings in batch. Reason: extracting a version inside quotes in
    rem batch needs tokens^=3delims^=^" style escaping, which breaks easily in
    rem if/for blocks (observed returning empty, causing false consistency
    rem failures). One re.match in Python does it, and it matches the parsing
    rem rules used by check_bin_version.py.
    set "CURVER="
    for /f "delims=" %%v in ('%PY% "%HERE%check_bin_version.py" --header-only 2^>nul') do set "CURVER=%%v"
    echo [INFO] version.h currently says: !CURVER!
    echo.
    echo --------------------------------------------
    echo   1/3 Building firmware  ^(idf.py build^)
    echo --------------------------------------------
    echo.

    rem Prefer an already-exported idf.py; otherwise locate an IDF environment
    set "IDFPY="
    where idf.py >nul 2>nul && set "IDFPY=idf.py"
    if not defined IDFPY (
        rem This project's IDF root: esp-idf-v5.5.1
        if exist "D:\ESP32-IDF\esp-idf-v5.5.1\export.bat" (
            set "IDF_PATH=D:\ESP32-IDF\esp-idf-v5.5.1"
            set "IDFPY=1"
        )
    )
    if not defined IDFPY (
        echo [ERROR] idf.py not found and D:\ESP32-IDF\esp-idf-v5.5.1\export.bat is missing.
        echo         Run IDF's export.bat manually once, then rerun this script.
        echo.
        pause
        exit /b 1
    )

    cd /d "%PROJ%"
    rem You MUST reconfigure before build, not just build.
    rem
    rem   version.h's SNAP_FW_VERSION is read at CMake *configure* time and passed
    rem   to project(... VERSION ...), which ends up in the app image header as
    rem   esp_app_desc_t.version.
    rem
    rem   Running only "idf.py build": ninja notices version.h changed and
    rem   recompiles its dependents (the banner changes), but it does NOT rerun
    rem   CMake configure -- PROJECT_VER in CMakeCache is still the old value, so
    rem   the image header version does not move. Symptoms:
    rem     - the compile log looks fine and the artifact mtime is new
    rem     - publish_firmware.py still reads the old version
    rem     - /ota.json reports the old version and the device stays "up to date"
    rem   This is very hard to spot because everything except the version
    rem   "succeeds".
    if "!IDFPY!"=="1" (
        rem Export inside a child cmd so the current environment stays clean
        call "D:\ESP32-IDF\esp-idf-v5.5.1\export.bat" >nul 2>nul
        idf.py reconfigure build
    ) else (
        idf.py reconfigure build
    )
    if errorlevel 1 (
        echo.
        echo [ERROR] Build failed; publishing and starting are skipped.
        echo         Fix the compile errors and rerun this script.
        echo.
        pause
        exit /b 1
    )

    rem ---- post-build check: image header version must equal version.h ----
    rem "Build succeeded" is not enough -- the CMake cache trap above is exactly
    rem "succeeded but the version never changed". Read the built image header
    rem and compare against version.h; stop on mismatch.
    for /f "delims=" %%o in ('%PY% "%HERE%check_bin_version.py" --quiet 2^>nul') do set "BUILDVER=%%o"
    if not "!BUILDVER!"=="!CURVER!" (
        echo.
        echo [ERROR] Image header version does not match version.h!
        echo         version.h  : !CURVER!
        echo         image hdr  : !BUILDVER!
        echo         This means CMake was not reconfigured. Fix it with:
        echo           cd /d "%PROJ%"
        echo           rmdir /s /q build
        echo         then rerun this script (full rebuild, slower but clean).
        echo.
        pause
        exit /b 1
    )
    echo [OK] Image header version = !BUILDVER!, matches version.h.
    cd /d "%HERE%"
    echo.
    echo [OK] Build finished.
    echo.
)

rem ---------- 3. publish firmware to firmware\ ----------
echo --------------------------------------------
echo   2/3 Publishing firmware  ^(publish_firmware.py^)
echo --------------------------------------------
echo.

if "!SKIP_BUILD!"=="1" (
    rem Even without building, firmware\ must not be empty or /ota.json 404s
    dir /b "%HERE%firmware\*.bin" >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] No .bin in firmware\; /ota.json would return 404.
        echo         Drop the -nobuild flag and rerun so it builds first.
        echo.
        pause
        exit /b 1
    )
    echo [SKIP] Reusing the existing .bin in firmware\.
) else (
    %PY% "%HERE%publish_firmware.py"
    if errorlevel 1 (
        echo.
        echo [ERROR] Publish failed. Usual cause: no .bin under build (build did not succeed).
        echo.
        pause
        exit /b 1
    )
)
echo.

rem ---------- 4. start the server ----------
echo --------------------------------------------
echo   3/3 Starting server  ^(server.py^)
echo --------------------------------------------
echo.

rem Check Flask
%PY% -c "import flask" >nul 2>nul
if errorlevel 1 (
    echo [INFO] Flask missing, installing automatically...
    %PY% -m pip install --quiet --disable-pip-version-check flask
    if errorlevel 1 (
        echo [ERROR] Flask install failed. Run it manually:  %PY% -m pip install flask
        echo.
        pause
        exit /b 1
    )
)

rem Port taken by an outside program (not our stale process)
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 (
    echo [ERROR] Port %PORT% is already in use, possibly by another program.
    echo         netstat -ano ^| findstr :%PORT%
    echo         taskkill /F /PID ^<the-PID-you-found^>
    echo.
    pause
    exit /b 1
)

%PY% "%HERE%launch.py" >nul 2>nul
if not exist "%PIDFILE%" (
    echo [ERROR] Launch failed, log: %LOGFILE%
    echo.
    pause
    exit /b 1
)
set /p NEWPID=<"%PIDFILE%"

rem Wait for the port to listen (timeout spins when input is redirected; use ping)
set /a WAITCNT=0
:waitloop
ping -n 2 127.0.0.1 >nul
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 goto running
set /a WAITCNT+=1
if !WAITCNT! lss 15 goto waitloop

echo [WARN] Port still not up after 15s. Log follows:
echo --------------------------------------------
type "%LOGFILE%"
echo --------------------------------------------
echo.
pause
exit /b 1

:running
echo [OK] Service started ^(PID=!NEWPID!^), listening on 0.0.0.0:%PORT%
echo.

rem ---------- 5. self-check ----------
echo --------------------------------------------
echo   Self-check
echo --------------------------------------------
echo.

echo [1] GET /ota.json  (this is what the device reads at boot)
curl -s -m 5 http://localhost:%PORT%/ota.json
echo.
echo.

echo [2] Contents of firmware\
dir /b "%HERE%firmware\*.bin" 2>nul
if errorlevel 1 echo   ^(empty^)
echo.

echo --------------------------------------------
echo   REST settings to enter on the device web page
echo --------------------------------------------
set "HASIP=0"
for /f "delims=" %%a in ('%PY% "%HERE%lan_ip.py" 2^>nul') do (
    echo   rest_host  = %%a
    set "HASIP=1"
)
if "!HASIP!"=="0" echo   rest_host  = ^<no LAN IP found, check ipconfig yourself^>
echo   rest_port  = %PORT%
echo   rest_path  = /report
echo   rest_cmd   = /lampcmd
echo   rest_tls   = off
echo.
echo   APP_OTA_CHECK_URL should point at  http://^<that-IP^>:%PORT%/ota.json
echo   ^(edit main\app_conf.h, then rebuild and flash once^)
echo.

rem ---------- 6. warn if app_conf.h hardcodes a different OTA host ----------
findstr /C:"APP_OTA_CHECK_URL" "%PROJ%\main\app_conf.h" | findstr /C:"192.168" >nul
if not errorlevel 1 (
    echo [NOTE] APP_OTA_CHECK_URL in app_conf.h hardcodes an IP. If it differs
    echo        from the rest_host printed above, the device cannot fetch the
    echo        manifest and will never update.
    echo.
)

echo   Web console:  http://localhost:%PORT%/
echo   Stop service: double-click stop.bat
echo   Status only:  double-click status.bat
echo.
echo Press any key to close this window (the service keeps running).
pause >nul
endlocal
