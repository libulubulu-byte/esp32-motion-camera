# 接线与引脚表

> **唯一权威来源是 [`../main/pins_config.h`](../main/pins_config.h)。**
> 本文档���它的可读对照版；两者不一致时以 `pins_config.h` 为准，并请修正本文档。

---

## 1. 摄像头 FPC 24Pin 座（`CAM_xxx` 丝印）

| 丝印 | GPIO | 宏 | 说明 |
|---|---|---|---|
| Y2 | 11 | `CAM_PIN_D0` | 数据位 0 |
| Y3 | 9  | `CAM_PIN_D1` | |
| Y4 | 8  | `CAM_PIN_D2` | |
| Y5 | 10 | `CAM_PIN_D3` | |
| Y6 | 12 | `CAM_PIN_D4` | |
| Y7 | 18 | `CAM_PIN_D5` | |
| Y8 | 17 | `CAM_PIN_D6` | |
| Y9 | 16 | `CAM_PIN_D7` | 数据位 7 |
| XCLK | 15 | `CAM_PIN_XCLK` | 主机输出时钟，20 MHz（OV5640 可到 24 MHz） |
| PCLK | 13 | `CAM_PIN_PCLK` | 像素时钟，摄像头→ESP32 |
| VSYNC | 6 | `CAM_PIN_VSYNC` | 帧同步 |
| HREF | 7 | `CAM_PIN_HREF` | 行有效 |
| SIOD | 4 | `CAM_PIN_SDA` | SCCB 数据 |
| SIOC | 5 | `CAM_PIN_SCL` | SCCB 时钟 |
| RESET | — | `CAM_PIN_RESET` = `-1` | **未从模块引出**，必须配 `-1` |
| PWDN | — | `CAM_PIN_PWDN` = `-1` | **未从模块引出**，必须配 `-1` |

⚠️ **D0..D7 顺序写反的后果**：`camera init failed 0x105`（探测不到）或画面花屏/错位。
不要凭"Y2 应该接 D0"以外的记忆调整，务必对着板子丝印核。

⚠️ SCCB 的 SDA/SCL 上拉：Freenove 板通常已带 4.7 kΩ 上拉。上电前可用万用表量
`SDA`/`SCL` 对 `3V3` 的阻值，正常应该是 4~5 kΩ；如果是开路（MΩ 级），
需要外挂 4.7 kΩ，否则表现为"确定接了摄像头但探测不到 PID"。

---

## 2. 板载 microSD 卡槽（SDMMC 1-bit）

| 丝印 | GPIO | 宏 |
|---|---|---|
| SD_CLK | 39 | `SD_PIN_CLK` |
| SD_CMD | 38 | `SD_PIN_CMD` |
| SD_DATA0 | 40 | `SD_PIN_DAT0` |

只使用 **1-bit** 模式（不用 DAT1/2/3，也不支持 SDSPI）。
1-bit 在 20 MHz 下约 2.5 MB/s，对 200 KB 级 JPEG 足够，且省出三个 GPIO。

⚠️ **39/40 同时是 JTAG 的 MTCK/MTDO** ⇒ 本工程**禁用 JTAG 调试**，调试只能靠 UART0。

---

## 3. 用户外设

| 功能 | GPIO | 宏 | 极性/说明 |
|---|---|---|---|
| PIR 输入 | 1 | `PIN_PIR` | 高/低有效由 `config.pir_active_level` 决定 |
| 用户按键 | **0** | `PIN_BUTTON` | **板载 BOOT0**；短按=人脸识别（有脸才上传），长按 5 s=恢复出厂 |
| 补光白光 LED | 21 | `PIN_FLASH` | 经 N-MOS 驱动，**高电平点亮** |
| 蜂鸣器 | 47 | `PIN_BUZZER` | 高电平响 |
| WS2812 | 48 | `PIN_LED_WS2812` | RMT 驱动，单总线 |
| 绿色 LED | 2 | `PIN_LED_GREEN` | GPIO 直驱（板载 `LED_ON`） |

### 3.1 按键为什么是 BOOT0 (GPIO0) —— ★ 三条硬约束

2026-09-28 从 GPIO14 改到 **BOOT0**（原 14 在那块板上没引出按键，按下去没反应）。

GPIO0 是 **strapping 脚**，改完必须守住这三条，否则会踩"烧完不跑"的坑：

1. **内部上拉常开、下拉必须关**（`button_hal.c` 的 `gpio_config`）。若同时开上下拉，
   分压可能把复位时的采样电平拖过阈值 ⇒ 芯片进 **UART 下载模式**，现象是
   "烧完不运行、串口只打 ROM 横幅"（对应本工程已知的 `cat /dev/ttyUSB0` 陷阱）。
2. **上电/复位那一刻别按着键**。按键是"按下才接地"，正常松手状态下 GPIO0 被内部上拉拉高，
   boot 模式不受影响。
3. **固件里有 2 s 上电静默窗口**（`BUTTON_BOOT_IGNORE_MS`）。ROM/bootloader 在启动早期会采样
   GPIO0，若不屏蔽会出现"一上电先自己拍一张"。窗口内检测到按下会打
   `boot window (2000ms) 内检测到按下 -> 已忽略`，看到这行说明该机制生效了。

> 复位后马上按 BOOT0 是**烧录动作**（按住再复位 = 进下载模式），这是硬件行为，固件无法改变。

---

## 4. 被占用 / 禁止使用的引脚

| GPIO | 占用者 | 后果 |
|---|---|---|
| 19, 20 | USB D- / D+ | 本工程不用原生 USB；串口日志走 UART0 |
| 33–37 | **Octal PSRAM** | 任何情况下不可当 GPIO 用，动了会直接崩 |
| 43, 44 | UART0 TX/RX | 串口日志，不要复用 |
| 45, 46 | Strapping（Boot / LOG） | 保持默认上下拉，代码**不驱动** |
| 41, 42 | JTAG MTMS/MTDI | 保留；39/40 是 MTCK/MTDO 且被 SD 占用 |

`pins_config.h` 里把这些定义成了 `PIN_RESERVED_*` 常量，
方便以后有人改引脚时在 review 阶段一眼看出冲突。

---

## 5. 供电注意

- 摄像头 + SD 卡 + Wi-Fi 同开时瞬时电流可达 **500 mA 以上**；
- OV5640 在 5MP 模式下峰值更高，建议 USB 线直连主机后置口，不要用无源 HUB；
- 出现 `BROWNOUT` 复位（串口 `reset=BROWNOUT`）先怀疑供电，不要先怀疑代码。
