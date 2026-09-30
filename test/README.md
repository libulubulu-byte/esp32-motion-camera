# test/ —— 可在 PC 上跑的回归测试

这里的东西**不参与固件构建**（`main/CMakeLists.txt` 只收 `main/` 下的文件），
纯粹是开发时用来验证纯逻辑的。没有依赖 ESP-IDF。

## version_guard_test.* —— 防"无限刷机"

`main/ota_check.c` 在比较版本前会先校验清单里的 `version` 字段格式，
只接受 `数字[.数字]...`。这个守卫挡掉的是一个很容易发生、且现象很迷惑的事故：

> IDF 在 `PROJECT_VER` 未定义时会用 **git describe** 生成版本号（形如
> `676dc691-dirty`）并写进镜像头。`app_version_parse()` 会把其中能读到的
> 数字当作版本段，于是它被解析成 `676.0.0` —— 恒大于本机的 `0.1.1`。
> 结果是**设备每次上电都判定"有新版本"**：下载、刷写、重启，新镜像的版本
> 依然是同一个字符串，于是再来一遍。表面上就是"设备不停地重启"。

两层防护：

1. 顶层 `CMakeLists.txt` 把镜像头版本强制对齐到 `main/version.h`（治本）；
2. `ota_check.c` 的格式守卫拒绝任何非 `数字[.数字]` 的版本（兜底）。

### 跑法

```powershell
# Python 版（任何机器都能跑，覆盖全部用例的行为）
cd examples/get-started/esp32S3_CAM/test
python version_guard_test.py

# C 版（语法/类型检查；本机没有 x86 gcc，只能交叉编译）
#   $gcc = "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe"
#   & $gcc -I..\main -Wall -Wextra -c version_guard_test.c -o $env:TEMP\vg.o
```

两份文件里的守卫逻辑是**逐行同构**的：C 版给编译器看，Python 版给机器跑。
**改动 `ota_check.c` 里的守工时，这两处和它三处要一起改。**

### 期望输出

```
ALL PASS (0 failure(s))
```

关键用例（**reject 那组就是防刷机的**）：

| 输入 | 期望 | 原因 |
|---|---|---|
| `0.1.1` / `0.1.2` / `1.2` / `3` / `1.2.3.4` | accept | 正常版本 |
| `676dc691-dirty` | **reject** | git describe，会被解析成 676 → 无限刷机 |
| `0.1.1-rc1` | reject | 预发布后缀 |
| `v0.1.1` | reject | 前导 v |
| `0.1.` | reject | 结尾点（曾被漏掉，测试抓到后修掉） |
| `0.1.1.2.3` | reject | 超过四段 |
| ``（空）/ `abc` / `0..1` | reject | 畸形 |
