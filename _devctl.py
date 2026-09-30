#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时设备控制工具（绕开 TUN 代理，绑定源接口直连设备）。

用法：
    python _devctl.py log [正则]        # 打印设备日志（可过滤）
    python _devctl.py status            # 打印 /api/status
    python _devctl.py cfg '<json>'      # POST /api/config
    python _devctl.py post <path>       # POST 一个空 body
    python _devctl.py detect <jpg>      # POST /api/detect_raw 注入图片
"""
import json
import re
import sys

import _devreq


def show(h, b, limit=1200):
    print(h.splitlines()[0] if h else "(no response)")
    print(b.decode("utf-8", "replace")[:limit])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]

    if cmd == "log":
        h, b = _devreq.request("GET", "/api/log")
        try:
            lines = json.loads(b.decode("utf-8", "replace"))
        except Exception:
            show(h, b, 3000)
            return
        pat = sys.argv[2] if len(sys.argv) > 2 else None
        print("log lines: %d" % len(lines))
        for ln in lines:
            if not pat or re.search(pat, ln):
                print(ln)

    elif cmd == "status":
        h, b = _devreq.request("GET", "/api/status")
        show(h, b, 2500)

    elif cmd == "cfg":
        h, b = _devreq.request("POST", "/api/config",
                               sys.argv[2].encode(), "application/json")
        show(h, b, 800)

    elif cmd == "post":
        h, b = _devreq.request("POST", sys.argv[2], b"", "application/json")
        show(h, b, 800)

    elif cmd == "detect":
        with open(sys.argv[2], "rb") as f:
            data = f.read()
        h, b = _devreq.request("POST", "/api/detect_raw", data, "image/jpeg")
        show(h, b, 800)

    else:
        print("unknown cmd", cmd)
        print(__doc__)


if __name__ == "__main__":
    main()
