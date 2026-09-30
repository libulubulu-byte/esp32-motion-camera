#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：带重试地抓取 /api/status 关键字段。"""
import json
import time

import _devreq

for i in range(8):
    try:
        h, b = _devreq.request("GET", "/api/status", timeout=6)
        if not b:
            raise RuntimeError("empty body")
        j = json.loads(b.decode("utf-8", "replace"))
        w = j.get("wifi", {})
        s = j.get("system", {})
        sn = j.get("snapshot", {})
        print("ip=%s ssid=%s rssi=%s" % (w.get("ip"), w.get("ssid"), w.get("rssi")))
        print("uptime=%s armed=%s upload=%s time_valid=%s time=%s" %
              (j.get("uptime_ms"), j.get("armed"), s.get("upload_mode"),
               s.get("time_valid"), s.get("time")))
        # ★ 内部 RAM 才是会分配失败的那块（free_heap 含 PSRAM，会掩盖问题）
        print("heap_internal=%s (%.1fKB)  free_heap=%s (%.1fMB)  free_psram=%.1fMB  mqtt=%s" %
              (s.get("heap_internal"),
               (s.get("heap_internal") or 0) / 1024.0,
               s.get("free_heap"),
               (s.get("free_heap") or 0) / 1048576.0,
               (s.get("free_psram") or 0) / 1048576.0,
               s.get("mqtt")))
        print("human_gate=%s run=%s hit=%s miss=%s | snap ok=%s fail=%s" %
              (sn.get("human_gate_on"), sn.get("human_run"), sn.get("human_hit"),
               sn.get("human_miss"), sn.get("ok_count"), sn.get("fail_count")))
        print("queue ok=%s fail=%s last_err=%s" %
              (j.get("queue", {}).get("ok_count"), j.get("queue", {}).get("fail_count"),
               j.get("queue", {}).get("last_error")))
        break
    except Exception as e:
        print("retry %d: %s" % (i, e))
        time.sleep(2)
