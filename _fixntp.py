#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：把设备 ntp_server 指向 PC（192.168.137.1），然后重启设备。"""
import json
import time

import _devreq

payload = json.dumps({"ntp_server": "192.168.137.1"}).encode()
for i in range(6):
    try:
        h, b = _devreq.request("POST", "/api/config", payload, "application/json", timeout=10)
        print("POST /api/config ->", h.splitlines()[0] if h else "(no response)")
        print("  ", b.decode("utf-8", "replace")[:160])
        break
    except Exception as e:
        print("retry %d: %s" % (i, e))
        time.sleep(2)

time.sleep(1)
for i in range(6):
    try:
        h, b = _devreq.request("POST", "/api/reboot", b"", "application/json", timeout=10)
        print("POST /api/reboot ->", h.splitlines()[0] if h else "(no response)")
        break
    except Exception as e:
        print("reboot retry %d: %s" % (i, e))
        time.sleep(2)
