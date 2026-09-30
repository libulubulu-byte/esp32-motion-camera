# Freenove ESP32-S3-WROOM Snapshot Kit
#
# 依赖说明：
#  - driver        : esp32-camera 的 SCCB 需要 IDF 的 legacy i2c 驱动（driver/i2c.h）
#  - esp_driver_*  : IDF 5.2+ 拆分出的细粒度驱动组件
#  - esp_http_server / esp_http_client / esp_https_ota / esp-tls / mdns: 网络链路
#  - fatfs / sdmmc : SD 卡（SDMMC 1-bit，无需 SDSPI）
#  - littlefs      : /config.json 分区
#  - json          : cJSON
#  - esp_timer / nvs_flash / esp_wifi / esp_netif / lwip : 基础
#  - mbedtls       : OTA 的本地 MD5 预校验（校验通过才写分区）
set(HTML_SRCS
    "web/index.html"
)

idf_component_register(
    SRCS
        "main.c"
        "logger.c"
        "camera_hal.c"
        "sd_hal.c"
        "wifi_hal.c"
        "config_store.c"
        "trigger_fsm.c"
        "snapshot_pipeline.c"
        "upload_queue.c"
        "upload_http.c"
        "upload_telegram.c"
        "mqtt_hal.c"
        "web_server.c"
        "ota_hal.c"
        "ota_check.c"
        "app_version.c"
        "led_hal.c"
        "buzzer_hal.c"
        "button_hal.c"
        "time_sync.c"
        # 深睡 / 唤醒策略（PIR + BOOT0 + 定时器 三个唤醒源）
        "sleep_mgr.c"
        "human_stub.c"
        # ESP-WHO 人脸检测（CONFIG_SNAP_ENABLE_HUMAN_DETECT 打开时才真正启用）
        # ★ 必须是 .cpp：它 #include 的是 human_face_detect.hpp / dl_*.hpp 这类
        #   C++ 头，还用 new/delete 与 std::list。放 .c 里编译会报
        #       dl_define.hpp:5:10: fatal error: string: No such file or directory
        #   这是它长期编译不过、开关一直关着的真正原因，别再改回 .c。
        "human_detect.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_rmt
        esp_driver_uart
        esp_driver_i2c
        esp_driver_sdmmc
        fatfs
        littlefs
        esp_http_server
        esp_http_client
        esp_https_ota
        esp-tls
        # mqtt：mqtt_hal.c 里 #include "mqtt_client.h"。
        # ★ IDF 5.5 的组件检查是**无条件**的（不看 mqtt_hal.c 里的 #if），
        #   所以哪怕 CONFIG_SNAP_ENABLE_MQTT 没打开也必须在 PRIV_REQUIRES 里列上，
        #   否则报 "includes mqtt_client.h, provided by mqtt component(s)"。
        mqtt
        esp_netif
        esp_wifi
        esp_timer
        esp_event
        esp_psram
        spi_flash
        json
        mdns
        nvs_flash
        app_update
        mbedtls
        # ESP-WHO 人脸检测：human_detect.c 里 #include "human_face_detect.hpp"。
        # ★ 与上面的 mqtt 同理，IDF 的组件检查是无条件扫描源码的（不看 #if），
        #   所以即使用户把 CONFIG_SNAP_ENABLE_HUMAN_DETECT 关掉，这里也必须列上，
        #   否则报 "includes human_face_detect.hpp, provided by human_face_detect"。
        #   esp-dl 由 human_face_detect 传递依赖，不必显式列。
        human_face_detect
        esp-dl
)

# web/index.html 通过 EMBED_FILES 编进固件（web_server.c 用 _binary_index_html_start
# / _binary_index_html_end 符号引用）。注意：用 target_add_binary_data 而不是
# idf_component_register 的 EMBED_FILES，原因是要显式指定 TEXT 而不是 BINARY，
# 保证 .rodata 段按 char[] 处理，httpd_resp_send() 才能直接当成字符串用。
foreach(html ${HTML_SRCS})
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/${html}" TEXT)
endforeach()

# ---------------------------------------------------------------------------
# 只为本组件关掉 -Wformat-truncation
#
# IDF 在 tools/cmake/build.cmake 里对**所有组件**统一加了 "-Wall -Werror"，
# 而且 **没有**对应的 Kconfig 开关可以单独放行 format-truncation。
# 本工程大量出现"用户输入 %s 拼进小缓冲"的写法（设备名/SSID/文件路径），
# 截断是**预期且安全**的行为（snprintf 保证 '\0' 结尾），但 GCC 会把它升级成
# error 打断整条构建。这里只对本组件放行这一条，其它警告仍然按 error 处理，
# **不要**把 -Werror 整个关掉。
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format-truncation")

