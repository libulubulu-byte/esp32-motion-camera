#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：把设备的 NTP 服务器改成公共 IP，绕开被代理污染的 DNS。"""
import json

import _devreq

# 1) 先读当前配置
h, b = _devreq.request("GET", "/api/config")
print("GET /api/config ->", h.splitlines()[0] if h else "(no response)")
try:
    cfg = json.loads(b.decode("utf-8", "replace"))
    print("  ntp_server =", cfg.get("ntp_server"), "| timezone =", cfg.get("timezone"))
except Exception:
    print("  raw:", b.decode("utf-8", "replace")[:300])

# 2) 写 ntp_server = 公共 NTP IP（203.107.6.88 = ntp.aliyun.com）
payload = json.dumps({"ntp_server": "203.107.6.88"}).encode()
h2, b2 = _devreq.request("POST", "/api/config", payload, "application/json", timeout=25)
print("POST /api/config ->", h2.splitlines()[0] if h2 else "(no response)")
print("  body:", b2.decode("utf-8", "replace")[:300])

# 3) 复核
h3, b3 = _devreq.request("GET", "/api/config")
print("GET /api/config ->", h3.splitlines()[0] if h3 else "(no response)")
try:
    cfg3 = json.loads(b3.decode("utf-8", "replace"))
    print("  ntp_server =", cfg3.get("ntp_server"))
except Exception:
    print("  raw:", b3.decode("utf-8", "replace")[:300])
