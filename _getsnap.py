#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：取回设备最近一帧（/api/last_snapshot），存成 jpg 便于查看相机看到什么。"""
import os

import _devreq

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_inject", "device_last.jpg")

h, b = _devreq.request("GET", "/api/last_snapshot", timeout=20)
line = h.splitlines()[0] if h else "(no response)"
print("status:", line)

if b[:2] == b"\xff\xd8":
    with open(OUT, "wb") as f:
        f.write(b)
    print("saved  : %s (%d bytes)" % (OUT, len(b)))
else:
    print("body   :", b.decode("utf-8", "replace")[:300])
