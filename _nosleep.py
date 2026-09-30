#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：等设备起来后立刻把 sleep_enable 置 0（防止它 30s 后自己睡掉、失联）。

用法：
    python _nosleep.py            # 置 0（调试期保持清醒）
    python _nosleep.py 1          # 置 1（恢复"做完事就睡"）
"""
import json
import sys
import time

import _devreq

want = sys.argv[1] if len(sys.argv) > 1 else "0"

# 等设备起来（深睡唤醒后要重跑 bootloader + WiFi 重关联，8~12s 是常态）
time.sleep(10)

for i in range(20):
    try:
        h, b = _devreq.request("POST", "/api/config",
                               json.dumps({"sleep_enable": int(want)}).encode(),
                               "application/json", timeout=6)
        print("POST sleep_enable=%s ->" % want, h.splitlines()[0] if h else "(no response)")
        break
    except Exception as e:      # noqa: BLE001
        print("retry %d: %s" % (i, e))
        time.sleep(2)

# 复核
for i in range(6):
    try:
        h, b = _devreq.request("GET", "/api/config", timeout=6)
        j = json.loads(b.decode("utf-8", "replace"))
        print("sleep_enable=%s sleep_poll_ms=%s sleep_max_awake_ms=%s" %
              (j.get("sleep_enable"), j.get("sleep_poll_ms"), j.get("sleep_max_awake_ms")))
        break
    except Exception as e:      # noqa: BLE001
        print("cfg retry %d: %s" % (i, e))
        time.sleep(2)
