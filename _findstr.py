#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：在 ESP-IDF 的 WiFi 静态库里定位 "m f null" 这条日志的出处与上下文。

为什么要查：真机串口反复出现 `W (12581) wifi:m f null`，
需要确认它是"库里的调试打印"还是"我们代码的问题"，不能靠猜。
"""
import os

ROOT = r"D:\ESP32-IDF\esp-idf-v5.5.1"
NEEDLE = b"m f null"

hits = 0
for dirpath, _dirnames, filenames in os.walk(ROOT):
    # 只扫 esp_wifi 的预编译库目录，避免把整个 IDF 走一遍
    if "esp_wifi" not in dirpath.replace("\\", "/"):
        continue
    for fn in filenames:
        if not fn.endswith(".a"):
            continue
        p = os.path.join(dirpath, fn)
        try:
            with open(p, "rb") as f:
                data = f.read()
        except OSError:
            continue
        start = 0
        while True:
            i = data.find(NEEDLE, start)
            if i < 0:
                break
            hits += 1
            lo = max(0, i - 80)
            hi = min(len(data), i + 80)
            chunk = data[lo:hi]
            # 只保留可打印字符，便于读上下文
            printable = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print("FILE :", p)
            print("OFF  :", i)
            print("CTX  :", printable)
            print("-" * 70)
            start = i + 1
            if hits > 12:
                break
    if hits > 12:
        break

if hits == 0:
    print("not found in esp_wifi libs")
