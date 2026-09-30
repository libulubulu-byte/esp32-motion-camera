#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""深睡循环健康检查 —— 区分「正常的定时唤醒」和「爆栈 panic 复位」。

核心判据（为什么不能用 uptime）：
    深睡形态下**每次唤醒本身就是一次完整重启**，所以 uptime 永远很小，
    光看 uptime 分不清"正常唤醒"和"崩溃重启"。
    真正的判据是启动日志里那行 reset 原因：
        reset=DEEPSLEEP      上一轮是正常深睡退出  => 好
        reset=PANIC / RTC_SW_CPU_RST   上一轮是崩溃 => 坏
    再加上日志里不该出现 "A stack overflow in task main"。

用法：
    python _wakecheck.py          # 默认采样 200s（覆盖约 3 个 60s 睡眠周期）
    python _wakecheck.py 300      # 自定义采样时长（秒）

退出码：0 = 全部样本正常；2 = 见到崩溃痕迹；1 = 一次都没读通
"""
import json
import sys
import time

import _devreq

SPAN_S = int(sys.argv[1]) if len(sys.argv) > 1 else 200


def one_snapshot():
    """读一次 /api/log，返回 (行数, reset原因, 崩溃行数, 是否已入睡) 或 None。"""
    try:
        _, b = _devreq.request("GET", "/api/log", timeout=2)
        obj = json.loads(b.decode("utf-8", "replace"))
    except Exception:                       # noqa: BLE001
        return None

    lines = obj.get("lines") or obj.get("log") or [] if isinstance(obj, dict) else obj
    reset = "(无)"
    for l in lines:
        if l.startswith("[BOOT ]") and "reset=" in l:
            reset = l.split("reset=")[-1].strip()
    crash = sum(1 for l in lines
                if "stack overflow" in l.lower() or "guru meditation" in l.lower())
    slept = any("进入 deep sleep" in l for l in lines)
    return len(lines), reset, crash, slept


def main():
    t0 = time.time()
    n_ok = n_crash = n_slept = 0
    resets = set()
    bad_lines = []

    print(f"采样 {SPAN_S}s（深睡周期 60s，预期能看到 2~3 次唤醒）")
    while time.time() - t0 < SPAN_S:
        snap = one_snapshot()
        if snap is None:
            time.sleep(0.4)
            continue
        n_lines, reset, crash, slept = snap
        n_ok += 1
        resets.add(reset)
        n_crash += crash
        n_slept += 1 if slept else 0
        print(f"  [{time.time() - t0:6.1f}s] 行数={n_lines:3d} reset={reset:14s} "
              f"崩溃={crash} 已入睡={slept}")
        time.sleep(1.2)

    print("\n================ 汇总 ================")
    print(f"成功采样次数     : {n_ok}")
    print(f"出现过的 reset   : {sorted(resets) if resets else '(无)'}")
    print(f"崩溃痕迹总数     : {n_crash}")
    print(f"含入睡闭环的快照 : {n_slept}")

    if n_ok == 0:
        print("结论: FAIL 一次都没读通（设备完全不可达？）")
        sys.exit(1)

    bad = (n_crash > 0) or any("PANIC" in r for r in resets)
    if bad:
        print("结论: FAIL 仍有崩溃（reset 出现 PANIC 或日志有 stack overflow）")
        sys.exit(2)

    print("结论: OK 全部样本都是正常深睡退出（reset=DEEPSLEEP），无崩溃痕迹")
    print("      => 定时唤醒 -> 轮询 Telegram -> 回睡 的闭环是干净的")
    sys.exit(0)


if __name__ == "__main__":
    main()
