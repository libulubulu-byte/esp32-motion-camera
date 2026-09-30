# 传感器切换（编译期）

任务书要求：**同一套代码编译出三个固件**，禁止运行时探测切换。
本工程用 **两个必须同步修改的开关** 实现该要求。

---

## 1. 两个开关

| 开关 | 位置 | 作用 |
|---|---|---|
| `CONFIG_CAM_OV2640` / `CONFIG_CAM_OV3660` / `CONFIG_CAM_OV5640` | `main/Kconfig.projbuild` 的 `choice CAMERA_SENSOR` | 决定 **代码走哪个分支**（上限帧率、PID、AF 能力、日志名） |
| `CONFIG_OV2640_SUPPORT` / `CONFIG_OV3660_SUPPORT` / `CONFIG_OV5640_SUPPORT` | `managed_components/espressif__esp32-camera/Kconfig` | 决定 **固件里链进哪个驱动** |

### ★★ 最容易踩的坑：`*_SUPPORT` 默认全是 `y`

`esp32-camera` 2.1.7 的每一个 `config XXX_SUPPORT` 都是：

```kconfig
config OV2640_SUPPORT
    bool "Support OV2640 2MP"
    default y          # ← 默认全开
```

而 `sdkconfig.defaults` **只覆盖"写出来的键"**。没写到的键会保持它自己的 Kconfig 默认值
`y`。所以"只关掉我记得的那几个"是**错的** —— 实测 `OV3640_SUPPORT`（一个我们根本
不打算支持的型号）会静默留在固件里。

**正确做法**：把**除了选中那一个之外的全部驱动显式关掉**。

**验证方法**（结果应当**只有一行**）：

```powershell
Select-String sdkconfig -Pattern '^CONFIG_OV\w+_SUPPORT=y'
```

实测瘦身效果：`ov2640_optional` 从 1 321 936 → **1 302 448** 字节（**−19.5 KB**）。

---

## 2. 各型号差异

| 型号 | 像素 | PID | `SENSOR_MAX_FS` | AF | 备注 |
|---|---|---|---|---|---|
| OV2640 | 2 MP | `0x2642` | `FRAMESIZE_UXGA` (1600×1200) | 无 | **默认**，最稳 |
| OV3660 | 3 MP | `0x3660` | `FRAMESIZE_QXGA` (2048×1536) | 无 | 无 AF |
| OV5640 | 5 MP | `0x5640` | `FRAMESIZE_QSXGA` (2560×1920) | 有（`CONFIG_CAMERA_AF_SUPPORT`，默认 n） | 5MP 单帧 PSRAM 压力大 |

### ★ `FRAMESIZE_3MP` 不存在

OV3660 的 3 MP 对应的是 **`FRAMESIZE_QXGA`**（2048×1536）。
esp32-camera 2.1.7 的 `framesize_t` 里**没有 `FRAMESIZE_3MP`**，
有的是：

- `FRAMESIZE_P_3MP` — 864×1536（竖屏，不是 OV3660 的 3MP）
- `FRAMESIZE_QXGA` — 2048×1536 ← **这才是 OV3660 的 3MP**

写 `FRAMESIZE_3MP` 会得到 `error: 'FRAMESIZE_3MP' undeclared`。

---

## 3. 切换步骤（三处必须同步）

以 OV2640 → OV5640 为例：

1. **`main/Kconfig.projbuild` 之外的 sdkconfig**（或 `idf.py menuconfig`）：
   把 choice 从 `CONFIG_CAM_OV2640=y` 改成 `CONFIG_CAM_OV5640=y`。
2. **`sdkconfig.defaults` 的"传感器驱动裁剪"段**：
   `CONFIG_OV5640_SUPPORT=y`，并把 `CONFIG_OV2640_SUPPORT` 改成 `is not set`。
3. 跑一次验证：
   ```powershell
   Select-String sdkconfig -Pattern '^CONFIG_OV\w+_SUPPORT=y'   # 必须只有一行
   Select-String sdkconfig -Pattern '^CONFIG_CAM_OV\w+=y'      # 必须只有一行
   ```

> 漏掉第 2 步的典型症状：代码按 OV5640 配 5 MP，
> 但固件里根本没有 OV5640 驱动 → 运行时 `camera init failed`，
> 而日志看起来一切正常。这就是"看起来改了其实没改"。

---

## 4. 帧率上限与 clamp

`camera_hal.c` 按型号定义 `SENSOR_MAX_FS`；运行时若配置的 `frame_size`
超过该上限，**自动 clamp 到上限并打警告**：

```
[CAM] frame_size 15 > max 13 (OV2640) -> clamp
```

OV5640 另有额外保护：单帧 > `FRAMESIZE_XGA` 时提示 PSRAM 队列风险并降到 XGA。

---

## 5. 编译矩阵里的三组

`tools/build_matrix.ps1` 中的 `ov2640_optional` / `ov3660_optional` /
`ov5640_optional` 三组分别对应三个固件，各自覆盖：

```
tools/cfg_overrides/sdkconfig.ov2640_optional.defaults   → CONFIG_CAM_OV2640=y  + OV2640_SUPPORT=y
tools/cfg_overrides/sdkconfig.ov3660_optional.defaults   → CONFIG_CAM_OV3660=y  + OV3660_SUPPORT=y
tools/cfg_overrides/sdkconfig.ov5640_optional.defaults   → CONFIG_CAM_OV5640=y  + OV5640_SUPPORT=y
```

矩阵输出里的 `VERIFY:` 行就是上面两条验证命令的自动版。
