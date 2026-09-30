#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：带重试地抓取设备日志尾部。"""
import json
import sys
import time

import _devreq

N = int(sys.argv[1]) if len(sys.argv) > 1 else 15

for i in range(8):
    try:
        h, b = _devreq.request("GET", "/api/log", timeout=6)
        if not b:
            raise RuntimeError("empty body")
        lines = json.loads(b.decode("utf-8", "replace"))
        for ln in lines[-N:]:
            print(ln)
        break
    except Exception as e:
        print("retry %d: %s" % (i, e))
        time.sleep(2)
