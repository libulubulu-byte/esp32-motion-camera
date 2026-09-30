#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时调试工具：一键改设备配置项（绕开 PowerShell 的 JSON 转义地狱）。

用法：
    python _cfgset.py sleep_enable=0
    python _cfgset.py sleep_enable=0 sleep_poll_ms=60000
    python _cfgset.py human_score_pct=20          # 数值自动按 int 写
    python _cfgset.py wifi_ssid=MyAP              # 非数值按字符串写

为什么单独写一个：
    在 PowerShell 里传 {"a":1} 得写成 '{\"a\":1}'，引号层级一多就出错，
    已经被坑过好几次（命令被 cmd 接管、反斜杠被吃掉……）。
    这里改成 key=value 参数，JSON 由脚本自己拼，shell 里一个引号都不用。

行为（刻意如此）：
    1) 先 GET /api/config 打印**改前**值
    2) POST 目标项（带重试：设备可能正在深睡，HTTP 会超时）
    3) 再 GET 一次打印**改后**值 —— 只信"读回来"的值，
       不信 {"saved":true}：设备有过"saved 为真但键没落盘"的先例。
"""
import json
import sys
import time

import _devreq


def get_cfg(retries=6):
    last = None
    for _ in range(retries):
        try:
            _, b = _devreq.request("GET", "/api/config", timeout=6)
            return json.loads(b.decode("utf-8", "replace"))
        except Exception as e:                      # noqa: BLE001
            last = e
            time.sleep(2)
    raise SystemExit(f"读取 /api/config 失败（设备在深睡？先动一下 PIR 唤醒它）: {last}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("用法: python _cfgset.py key=value [key2=value2 ...]")

    body = {}
    for kv in sys.argv[1:]:
        if "=" not in kv:
            raise SystemExit(f"参数格式应为 key=value: {kv!r}")
        k, v = kv.split("=", 1)
        try:
            body[k] = int(v)          # 配置项绝大多数是 int
        except ValueError:
            body[k] = v               # wifi_ssid / http_url 这类是字符串
    print("将写入:", json.dumps(body, ensure_ascii=False))

    before = get_cfg()
    for k in body:
        print(f"  改前 {k} = {before.get(k, '(无此键)')}")

    payload = json.dumps(body).encode()
    for i in range(6):
        try:
            _, b = _devreq.request("POST", "/api/config", payload,
                                   "application/json", timeout=8)
            print("POST ->", b.decode("utf-8", "replace")[:200])
            break
        except Exception as e:                      # noqa: BLE001
            print(f"  第 {i + 1} 次 POST 失败: {e}")
            time.sleep(2)
    else:
        raise SystemExit("写入失败：设备一直不在线")

    after = get_cfg()
    print("--- 读回校验 ---")
    bad = 0
    for k, v in body.items():
        got = after.get(k)
        if got != v:
            bad += 1
        print(f"  {'OK ' if got == v else '!! 未生效'} {k}: "
              f"{before.get(k)} -> {got} (期望 {v})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
