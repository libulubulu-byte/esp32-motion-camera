/**
 * @file pins_config.h
 * @brief Freenove ESP32-S3-WROOM 开发板固定引脚表（按开发板丝印 CAM_/SD_ 填写）
 *
 * 【唯一引脚来源】全工程禁止在其它 .c/.h 里硬编码任何 GPIO 号，
 * 一律 #include "pins_config.h" 取宏。修改引脚只允许改本文件。
 *
 * 数据手册/丝印对照见 docs/wiring.md。
 */

#pragma once

/* ============================ 摄像头 FPC 24Pin 座 CAM_xxx ============================ */
/* D0..D7 <-> Y2..Y9 的对应关系严格按开发板丝印，写反会 camera init failed(0x105) 或花屏 */
#define CAM_PIN_D0     11
#define CAM_PIN_D1     9
#define CAM_PIN_D2     8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_XCLK   15
#define CAM_PIN_PCLK   13
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
/* SCCB(I2C)：板上通常已带 4.7K 上拉；上电前建议实测 SDA/SCL 对 3V3 的阻值 */
#define CAM_PIN_SDA    4
#define CAM_PIN_SCL    5
/* RESET / PWDN 未从模块引出：必须配 -1 */
#define CAM_PIN_RESET  (-1)
#define CAM_PIN_PWDN   (-1)

/* ======================== 板载 microSD 卡槽 SD_CLK/CMD/DATA ======================== */
/* 只支持 SDMMC 1-bit：JTAG 是 GPIO39/40/41/42，其中 39/40 被 SD 占用 ⇒ 禁止使能 JTAG */
#define SD_PIN_CLK     39
#define SD_PIN_CMD     38
#define SD_PIN_DAT0    40

/* ============================== 用户外设 ============================== */
#define PIN_PIR        1   /* PIR 触发输入，高/低有效由 config 决定 */

/* ★ 2026-09-28 按键从 GPIO14 改到 **BOOT0 = GPIO0**。
 *
 * 为什么：现场开发板上 GPIO14 没有引出可用按键，只有板载 BOOT0（丝印 BOOT/BOOT0，
 * 一端接 GPIO0、一端接 GND，同时是 strapping 脚）。原来的 14 按下去没反应，
 * 就是因为那块板上根本没有这个键。
 *
 * ⚠️ GPIO0 是 **strapping 脚**（决定 boot 模式），三条硬约束：
 *   1. **内部上拉必须常开**：GPIO0 若在复位瞬间被拉低，芯片会进 UART 下载模式而不是跑固件。
 *      按键是"按下才接地"，所以只要别在**上电/复位那一刻**一直按着就没问题。
 *   2. **不能配上拉电阻的强下拉**：gpio_config 里 pull_down_en 必须 DISABLE。
 *   3. **上电后要有静默窗口**：bootloader 与 ROM 在启动早期会读这个脚，
 *      代码里 BUTTON_BOOT_IGNORE_MS 在前 N 秒内**不认按键**，避免"松手时残留电平"
 *      被判成一次短按（实测会出现"一上电就自己拍一张"）。
 *
 * 极性不变：低有效（按下接地，靠内部上拉拉高），与 14 时完全一致。 */
#define PIN_BUTTON     0   /* 按键 = 板载 BOOT0；短按人脸识别，长按 5s 恢复出厂 */
#define PIN_FLASH      21  /* 补光白光 LED，经 N-MOS 驱动，高电平点亮 */
#define PIN_BUZZER     47  /* 蜂鸣器（可选），高电平响 */
#define PIN_LED_WS2812 48  /* 板载 WS2812，RMT 驱动 */
#define PIN_LED_GREEN  2   /* 板载 LED_ON，GPIO 直驱 */

/* ============================== 被占用 / 禁止使用 ==============================
 * 19,20   USB D-/D+（原生 USB，本工程不用；串口日志走 UART0）
 * 33-37   Octal PSRAM，任何情况下不可当 GPIO 用
 * 43,44   UART0 TX/RX，串口日志
 * 45,46   Strapping（Boot / LOG），保持默认上下拉，代码不驱动
 * 41,42   JTAG（MTMS/MTDI）；39,40 是 JTAG 的 MTCK/MTDO 且被 SD 占用 ⇒ 禁用 JTAG
 * ============================================================================== */

/* ------------------------------- 派生约束 ------------------------------- */
/* SD 占用 38/39/40、CAM 占用 4-18、外设占用 1/2/14/21/47/48 —— 编译期做一次自检，
 * 防止以后有人改引脚时不小心撞上 PSRAM/USB/UART/Strapping。 */
#define PIN_RESERVED_USB_DM     19
#define PIN_RESERVED_USB_DP     20
#define PIN_RESERVED_PSRAM_0    33
#define PIN_RESERVED_PSRAM_1    37
#define PIN_RESERVED_UART0_TX   43
#define PIN_RESERVED_UART0_RX   44
#define PIN_RESERVED_STRAP_BOOT 45
#define PIN_RESERVED_STRAP_LOG  46
