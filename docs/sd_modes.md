# SD 三种模式与状态机

引脚（`main/pins_config.h`）：`SD_PIN_CLK=39` / `SD_PIN_CMD=38` / `SD_PIN_DAT0=40`，
**SDMMC 1-bit**，`esp_vfs_fat_sdmmc_mount()` 挂到 `/sdcard`。

---

## 1. 三种编译期模式

由 `main/Kconfig.projbuild` 的 `choice SD_MODE` 决定，符号：
`CONFIG_SD_MODE_NONE` / `CONFIG_SD_MODE_OPTIONAL` / `CONFIG_SD_MODE_REQUIRED`。

| 模式 | 行为 | 无卡时 |
|---|---|---|
| **NONE** | 完全不挂载；`sd_hal` 全部接口返回 `ESP_ERR_NOT_SUPPORTED` | 正常启动，全部走 PSRAM 队列 |
| **OPTIONAL**（默认） | 有卡写卡；失败自动降级；30 s 自动重挂载 | **正常启动**，降级到 PSRAM 队列 |
| **REQUIRED** | 挂载失败视为致命 | 红灯常亮 + 蜂鸣器，只保留 safe web 配网，**不拍照** |

> **任务书硬性要求**：默认配置（OPTIONAL）下不插卡也必须能启动、进 AP 配网、拍照、上传。
> 这一条由 `ov2640_optional` / `ov2640_sd_none` 两组编译矩阵 + 真机验证覆盖。

运行期可用两个查询函数判断编译期模式，避免在业务代码里写 `#ifdef`：

```c
bool sd_hal_is_enabled(void);    /* false = NONE   */
bool sd_hal_is_required(void);   /* true  = REQUIRED */
```

---

## 2. 状态机

```c
typedef enum {
    SD_DISABLED = 0,   /* SD_MODE_NONE 或编译期关掉            */
    SD_UNKNOWN,        /* 尚未尝试挂载                          */
    SD_MOUNTING,       /* 正在挂载（含重试）                     */
    SD_OK,             /* 已挂载且可写                          */
    SD_FAIL,           /* 挂载/写失败，已降级到 PSRAM 队列        */
    SD_REMOVED,        /* 运行期检测到卡被拔掉                   */
    SD_LOWSPACE,       /* 剩余空间 < config.sd_low_space_mb      */
} sd_state_t;
```

人可读名字由 `sd_hal_state_str(st)` 给出，`/api/status` 与日志都用它。

### 降级触发条件

1. **挂载失败**（无卡 / 卡未格式化 / 接触不良）→ `SD_FAIL`，直接用 PSRAM 队列。
   日志：`[SD] mount fail err=0x%x -> degrade queue_mode=buffer retry=30s`
2. **连续 3 次写失败** → `SD_FAIL`（单次失败会重试，不会立刻降级）。
   日志：`[SD] write fail count>=3 state=FAIL -> degrade`
3. **运行期检测到卡被拔掉** → `SD_REMOVED` → `SD_FAIL`。

> `SD_REMOVED` 是 `sd_state_t` 里的枚举值，用于**区分**"从来没挂上"（`SD_FAIL`）
> 与"挂上后又掉了"（`SD_REMOVED`）—— 前者大概率没插卡，后者需要查接触/供电。

### 恢复

`SD_FAIL` / `SD_REMOVED` 之后每 **30 s** 自动尝试重挂载一次；
也可由 `POST /api/sd/remount` 手动触发（需 `X-Admin-Key`）。

重挂载成功的日志是 `[SD] remount ok recovered state=OK`（注意**不是** `remount ok`，
少一个词就 grep 不到）；`sd_status_t.retry_count` 累加。

---

## 3. 文件布局

```
/sdcard/snapshots/YYYY/MM/DD/HHMMSS_mmm.jpg
```

- 时间取自 SNTP；**时间未同步时**退化为开机毫秒数，文件名仍保证唯一；
- 写入采用 **先写 `.tmp` 再 `rename`**，避免掉电留下半个 JPEG；
- 目录层级自动创建（逐级 `mkdir`）。

---

## 4. 低空间清理策略（★ 不许删未上传的文件）

`config.sd_low_space_mb`（默认 **200 MB**）是水位线。
`sd_hal_check_space()` 触发清理时，**只清理"已被 upload_queue 标记为上传成功"的文件**。

机制是 `sd_hal_note_uploaded_ok(rel_path)`：上传成功后由上传模块回调登记，
形成一条**"上传成功水位线"**。清理时只删除水位线**之下**（更早）的文件。

这样设计的理由是：直接按"最旧目录"删除是最省事的写法，但它会把
**还没上传成功**的照片删掉 —— 对"抓拍上传"类设备来说这是数据丢失，不可接受。

日志（`sd_hal.c`）：

```
[SD] free=120MB < low=200MB -> LOWSPACE
[SD] low space free=120MB < 200MB -> cleanup (watermark=snapshots/2026/09/26/191500_000.jpg)
[SD] cleanup done free=198MB
```

> 如果卡里全是未上传的文件，清理会**什么都不删**（日志里 `cleanup done` 后的
> `free=` 几乎不变），此时正确的动作是修网络/修上传地址，而不是删数据。
> 判据看 `free=` 前后差值，不要只看有没有 `cleanup` 字样。

---

## 5. `/sdcard` 与 PSRAM 队列的分工

| | 有 SD | 无 SD（降级） |
|---|---|---|
| 抓拍 JPEG | 写 `/sdcard/snapshots/...`，队列只存**路径**（`path` 模式） | 队列直接存**数据**（`buffer` 模式） |
| `/api/last_snapshot` | 读 SD 文件 | 读 `snapshot_pipeline` 保留的 PSRAM 副本 |
| 掉电保留 | 是 | 否 |
| 队列容量 | 受卡容量限制 | 受 `config.max_queue_len`（默认 8）限制 |

队列模式由上传队列内部按 `sd_hal_state()` 自动选择，业务代码不需要关心。

---

## 6. 排错速查

| 现象 | 根因 / 处理 |
|---|---|
| `sdmmc` 挂载失败但卡是好的 | 卡未格式化为 FAT32；或用了 exFAT（需 `CONFIG_FATFS_LFN_HEAP` 且 IDF 默认不支持 exFAT） |
| 卡着 `SD_MOUNTING` 不往下走 | 卡座接触不良；换一张卡排除 |
| 写一次成功、之后全失败 | 卡写保护或劣质卡；看 `sd_status_t.last_error` |
| `SD_REQUIRED` 但想临时跳过 | 换成 `OPTIONAL` 重编，别在代码里绕过 |
| `SD_LOWSPACE` 但空间没释放 | 卡里全是**未上传**文件，这是**设计行为**（见 §4） |
| 插卡后 Wi-Fi 变差 | 供电不足；SD 写峰值 + Wi-Fi 发射同时发生，用后置 USB 口 |
