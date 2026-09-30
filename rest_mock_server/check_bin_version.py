#!/usr/bin/env python3
r"""读固件镜像头里的版本号，并和 main/version.h 对一眼。

为什么需要它：
    version.h 的 SNAP_FW_VERSION 只在 CMake **配置阶段**被读走、传给
    project(... VERSION ...)，最后进 app 镜像头的 esp_app_desc_t.version。
    只跑 idf.py build 而不重跑配置时，ninja 会重编依赖 version.h 的 .c 文件，
    但 CMakeCache 里的版本变量不变 —— 镜像头版本于是**纹丝不动**。
    这种情况下构建"成功"、产物 mtime 是新的、md5 也变了，唯独版本号没变，
    非常难看出来（之前就踩过：改 0.1.0 -> 0.1.2，build 完仍报 0.1.0）。

用法：
    python check_bin_version.py                    # 查 build\ 下的产物
    python check_bin_version.py path/to/x.bin      # 查指定文件
    python check_bin_version.py --quiet            # 只打印镜像头版本（给 bat 取用）
    python check_bin_version.py --header-only      # 只打印 version.h 的版本

退出码：0=一致；1=不一致或读不出来（bat 据此中止）。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, ".."))
BUILD_BIN = os.path.join(PROJ, "build", "esp32s3_snapshot_kit.bin")
VERSION_H = os.path.join(PROJ, "main", "version.h")

# 镜像布局：0x00 头部(0x20) | 0x20 esp_app_desc_t（其中 0x10 处是 version[32]）
VER_OFFSET = 0x30


def read_image_version(path):
    """从 app 镜像头读 esp_app_desc_t.version。非 ESP 镜像返回 None。"""
    try:
        with open(path, "rb") as f:
            head = f.read(VER_OFFSET + 32)
    except OSError:
        return None
    if len(head) < VER_OFFSET + 32 or head[:1] != b"\xe9":
        return None
    return head[VER_OFFSET:VER_OFFSET + 32].split(b"\x00")[0].decode(errors="replace")


def read_version_h():
    """从 version.h 抠出 SNAP_FW_VERSION 的字符串值。"""
    try:
        with open(VERSION_H, encoding="utf-8") as f:
            for line in f:
                m = re.match(
                    r'^[ \t]*#[ \t]*define[ \t]+SNAP_FW_VERSION[ \t]+"([^"]+)"', line
                )
                if m:
                    return m.group(1)
    except OSError:
        pass
    return None


def main():
    args = list(sys.argv[1:])
    quiet = "--quiet" in args
    header_only = "--header-only" in args
    args = [a for a in args if not a.startswith("--")]

    hdr_ver = read_version_h()

    if header_only:
        # 给 bat 的 for /f 用：只输出一行、无多余空白
        print(hdr_ver if hdr_ver else "")
        return 0 if hdr_ver else 1

    path = args[0] if args else BUILD_BIN
    bin_ver = read_image_version(path)

    if quiet:
        # bat 用 for /f 取这一行，注意：只能输出一行，且不能有多余空白
        print(bin_ver if bin_ver else "")
        return 0 if bin_ver else 1

    print(f"镜像文件 : {path}")
    print(f"镜像头版本: {bin_ver if bin_ver else '(读不出来，不是 app 镜像?)'}")
    print(f"version.h: {hdr_ver if hdr_ver else '(没找到 SNAP_FW_VERSION)'}")

    if not bin_ver:
        print("\n[失败] 读不出镜像头版本。")
        print("       多半是：还没构建，或这不是 app 镜像（bootloader/ota_data）。")
        return 1
    if bin_ver != hdr_ver:
        print(f"\n[失败] 不一致：镜像头={bin_ver}  version.h={hdr_ver}")
        print("       CMake 没重新配置。删掉 build 目录后全量重建：")
        print(f"         rmdir /s /q \"{os.path.join(PROJ, 'build')}\"")
        return 1

    print(f"\n[通过] 两处一致，都是 {bin_ver}。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
