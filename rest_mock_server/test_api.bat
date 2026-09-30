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
rem  Port must match start.bat (changed 2026-09-27 from 8080 to 8081)
setlocal

set "BASE=http://localhost:8081"

echo === 1. Device report POST /report ===
curl -s -X POST %BASE%/report -H "Content-Type: application/json" -d "{\"device\":\"esp32s3-test\",\"version\":\"1.0.0\",\"temperature\":25.4,\"humidity\":61.2,\"rssi\":-58,\"uptime\":1234,\"lamp\":\"OFF\"}"
echo.

echo === 2. Lamp command poll GET /lampcmd ^(expect 204, no command^) ===
curl -s -o nul -w "HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 3. Queue an ON from the console ===
curl -s -X POST %BASE%/api/lamp -H "Content-Type: application/json" -d "{\"state\":\"ON\"}"
echo.

echo === 4. Poll GET /lampcmd again ^(expect 200 + {"lamp":"ON"}^) ===
curl -s -w "  <- HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 5. Third poll ^(expect 204, command already consumed^) ===
curl -s -o nul -w "HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 6. Latest report GET /api/latest ===
curl -s %BASE%/api/latest
echo.

echo === 7. Stats GET /api/stats ===
curl -s %BASE%/api/stats
echo.

echo === 8. History GET /api/history ===
curl -s %BASE%/api/history
echo.

endlocal
