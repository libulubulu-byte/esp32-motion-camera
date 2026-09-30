@echo off
REM build_cfg.bat <name> [extra sdkconfig lines file]
REM 静默编译并把错误/警告汇总到 build_log_<name>.txt
powershell -NoProfile -Command "& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null; cd 'd:/ESP32-IDF/esp-idf-v5.5.1/examples/get-started/esp32S3_CAM'; idf.py build 2>&1 | Tee-Object -FilePath 'build_log_%1.txt' | Select-String -Pattern 'error:|warning:|FAILED|failed|Error' | Select-Object -First 120"
