#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：设置深睡测试参数。

用法：
    python _sleeptest.py 1 20000 15000    # enable poll_ms max_awake_ms
    python _sleeptest.py 1 60000 30000    # 恢复交付默认
    python _sleeptest.py 0 60000 30000    # 关掉深睡（调试期）
"""
import json
import sys

import _devreq

en = int(sys.argv[1]) if len(sys.argv) > 1 else 1
poll = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
maxaw = int(sys.argv[3]) if len(sys.argv) > 3 else 15000

payload = json.dumps({
    "sleep_enable": en,
    "sleep_poll_ms": poll,
    "sleep_max_awake_ms": maxaw,
}).encode()

import time

# 深睡形态下设备大部分时间在睡，必须重试到它醒来的那个窗口
for i in range(40):
    try:
        h, b = _devreq.request("POST", "/api/config", payload, "application/json", timeout=5)
        print("POST ->", h.splitlines()[0] if h else "(no response)")
        break
    except Exception as e:      # noqa: BLE001
        print("retry %d: %s" % (i, e))
        time.sleep(2)

for i in range(20):
    try:
        h, b = _devreq.request("GET", "/api/config", timeout=5)
        j = json.loads(b.decode("utf-8", "replace"))
        print("sleep_enable=%s sleep_poll_ms=%s sleep_max_awake_ms=%s" %
              (j.get("sleep_enable"), j.get("sleep_poll_ms"), j.get("sleep_max_awake_ms")))
        break
    except Exception as e:      # noqa: BLE001
        time.sleep(2)
