# Freenove ESP32-S3-WROOM Snapshot Kit
# 任务书要求 ESP-IDF v5.1.5 / v5.2.x；本机装的是 v5.5.1，代码只用 5.1 就有的稳定 API，
# 版本差异与验证结论见 README.md「与任务书的差异」章节。
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# 版本号的单一来源：main/version.h 的 SNAP_FW_VERSION
#
# 为什么必须显式设置（而不是让 IDF 自己去猜）：
#   IDF 在 PROJECT_VER 未定义时，会按 version.txt -> PROJECT_VERSION ->
#   **git describe** 的顺序取版本（tools/cmake/project.cmake:685-728），并把
#   结果写进 app image 头的 esp_app_desc_t.version。本工程原先没有前两者，
#   于是版本被 git 描述字符串（形如 "676dc691-dirty"）占据。
#
#   这在手动推 URL 升级时无害，但一旦启用"上电按版本号自动升级"就是致命的：
#   本机自报的版本是 version.h 的 "0.1.1"，而服务端从镜像头读出来的是
#   "676dc691-dirty" —— 二者永远不可能相等，且后者的首段数字（676）恒大于
#   前者（0），于是设备**每次上电都认为有新版本**，下载、刷写、重启，再判定
#   "还是有新版本"，形成无限刷机循环（而且刷进去的还是同一个镜像）。
#
#   所以这里把镜像头版本强制对齐到 version.h，两处同源。
#
# 校验而不是复制：直接从 version.h 解析版本，若解析不出来就构建失败，
# 绝不会静默地退回到 git describe（那正是上面那个循环的成因）。
# ---------------------------------------------------------------------------
set(_version_h "${CMAKE_CURRENT_LIST_DIR}/main/version.h")
if(NOT EXISTS "${_version_h}")
    message(FATAL_ERROR "main/version.h not found at ${_version_h}")
endif()
file(STRINGS "${_version_h}" _ver_line
     REGEX "^[ \t]*#[ \t]*define[ \t]+SNAP_FW_VERSION[ \t]+\"")
if(NOT _ver_line)
    message(FATAL_ERROR
        "main/version.h has no SNAP_FW_VERSION string, refusing to fall back to "
        "git describe (that breaks boot-time OTA version comparison).")
endif()
# 从 '#define SNAP_FW_VERSION "0.1.1"' 里抠出 0.1.1
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" SNAP_VERSION "${_ver_line}")
# 注意：这里的正则**刻意不用 {0,3}** —— CMake 的 if() 会把参数里的逗号当列表
# 分隔符，"{0,3}" 会被拆成 "{0" 和 "3}" 两段，MATCHES 于是永远不成立
# （已实测：内联与经变量传入的写法都失败）。用 * 表达同样语义且无逗号。
if(NOT SNAP_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR
        "SNAP_FW_VERSION \"${SNAP_VERSION}\" is not a dotted-numeric version. "
        "The boot-time OTA check compares it against the version read from the "
        "firmware image header, so it must be plain digits separated by dots.")
endif()
message(STATUS "Snapshot Kit version: ${SNAP_VERSION} (from main/version.h)")

# ---------------------------------------------------------------------------
# 顺手校验 SNAP_FW_VERSION_NUM 与字符串版本是否同步（只警告，不失败）。
#
# 这个整数当前没有任何代码在用（OTA 比的是字符串），所以不同步不影响功能，
# 但留着它就容易误导后来的人 —— 先前的注释就写错过（0x000100 被注释成 00.01.01）。
# 在这里对一眼，改版本时忘了同步会看到提示，而不是过几个月才发现。
# ---------------------------------------------------------------------------
file(STRINGS "${_version_h}" _ver_num_line
     REGEX "^[ \t]*#[ \t]*define[ \t]+SNAP_FW_VERSION_NUM[ \t]+")
if(_ver_num_line)
    string(REGEX REPLACE ".*0x([0-9A-Fa-f]+).*" "\\1" _ver_num_hex "${_ver_num_line}")
    # 把 "0.1.1" 拆成三段；不足三段补 0
    string(REPLACE "." ";" _ver_parts "${SNAP_VERSION}")
    list(GET _ver_parts 0 _v_major)
    if(_ver_parts MATCHES ";")
        list(GET _ver_parts 1 _v_minor)
    else()
        set(_v_minor 0)
    endif()
    list(LENGTH _ver_parts _v_len)
    if(_v_len GREATER 2)
        list(GET _ver_parts 2 _v_patch)
    else()
        set(_v_patch 0)
    endif()
    math(EXPR _ver_expect "0x${_v_major} * 65536 + 0x${_v_minor} * 256 + 0x${_v_patch}")
    math(EXPR _ver_have "0x${_ver_num_hex}")
    if(NOT _ver_have EQUAL _ver_expect)
        message(WARNING
            "SNAP_FW_VERSION_NUM (0x${_ver_num_hex}) 与 SNAP_FW_VERSION "
            "(\"${SNAP_VERSION}\") 不同步 —— 不影响 OTA（它比字符串），"
            "但建议把 _NUM 改成 0x${_ver_major}${_v_minor}${_v_patch} 以免误导。")
    endif()
endif()

# 本工程依赖 components/ 全部组件（camera 的 SCCB 走 legacy driver/i2c.h、
# littlefs、esp_http_server、mdns、led_strip 等），因此不开 MINIMAL_BUILD。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_snapshot_kit VERSION "${SNAP_VERSION}")
