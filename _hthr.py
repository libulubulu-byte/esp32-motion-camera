#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：读/写人形门控阈值 human_score_pct（%，5..95）。

用法：
    python _hthr.py          # 只读
    python _hthr.py 25       # 设为 25%
"""
import json
import sys
import time

import _devreq


def get_cfg():
    for i in range(6):
        try:
            h, b = _devreq.request("GET", "/api/config", timeout=6)
            if not b:
                raise RuntimeError("empty body")
            if b[:1] == b"{":
                return json.loads(b.decode("utf-8", "replace"))
        except Exception as e:
            print("retry %d: %s" % (i, e))
        time.sleep(2)
    return None


if len(sys.argv) > 1:
    val = int(sys.argv[1])
    payload = json.dumps({"human_score_pct": val}).encode()
    for i in range(6):
        try:
            h, b = _devreq.request("POST", "/api/config", payload,
                                   "application/json", timeout=8)
            print("POST ->", h.splitlines()[0] if h else "(no response)")
            break
        except Exception as e:
            print("retry %d: %s" % (i, e))
            time.sleep(2)

cfg = get_cfg()
if cfg:
    print("human_score_pct =", cfg.get("human_score_pct"))
else:
    print("(读配置失败)")
