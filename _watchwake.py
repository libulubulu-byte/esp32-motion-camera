#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：后台常驻，捕获**每一次唤醒**的来源并落盘。

为什么不能用串口：这块板子一旦挂住 COM9 就会被带进异常状态（实测两次）。
为什么不能"先查 status 再取 log"：定时唤醒的清醒窗口只有 ~7 秒
（起来 → 轮询一次命令 → 立刻回睡），两次 HTTP 串行很容易撞到窗口尾巴上。

所以这里**直接高频轮询 /api/log**（1 秒一次，超时 2 秒）：只要设备醒着
就拿得到那一次启动的完整日志；一旦 `wake=` / `deep sleep` / 人形判定
发生变化就记一行。设备睡着时请求会超时，属于正常。

用法： python _watchwake.py      （被 Start-Process 拉起则后台常驻）
输出： 追加写入 _wakewatch.log
"""
import json
import os
import time

import _devreq

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_wakewatch.log")


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(OUT, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def get_log():
    h, b = _devreq.request("GET", "/api/log", timeout=2)
    if not b:
        raise RuntimeError("empty")
    return json.loads(b.decode("utf-8", "replace"))


def main():
    log("=== wake watcher 启动（高频轮询 /api/log）===")
    prev = None
    last_ok = 0.0

    while True:
        try:
            lines = get_log()
        except Exception:
            time.sleep(1.0)
            continue

        last_ok = time.time()
        wake = next((l for l in lines if "wake=" in l), "(无 wake 行)")
        ds   = next((l for l in lines if "deep sleep" in l), "")
        ota  = next((l for l in lines if "跳过上电" in l), "")
        hum  = [l for l in lines if ("人形确认" in l or "非人形帧丢弃" in l)]
        tg   = [l for l in lines if ("sendPhoto" in l or "upload ok" in l)]

        key = (wake, ds, ota, tuple(hum), tuple(tg))
        if key != prev:
            log("======== 新窗口 ========")
            log("   " + wake)
            if ds:
                log("   " + ds)
            if ota:
                log("   " + ota)
            for l in hum:
                log("   " + l)
            for l in tg:
                log("   " + l)
            prev = key

        time.sleep(1.0)


if __name__ == "__main__":
    main()
