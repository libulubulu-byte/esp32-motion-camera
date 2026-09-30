#!/usr/bin/env python3
"""
parse_log.py —— 串口日志解析 / 验收判据提取（任务书第 6 节日志规范）

日志行格式（logger.c 输出）：
    I (12345) [BOOT] xxx            <- ESP_LOG 风格前缀，本工程复用 esp_log 输出
    W (12345) [SD  ] sd write fail
    E (12345) [CAM ] init failed err=0x105

本脚本做三件事：
  1. 归一化成 {t_ms, level, tag, msg} 列表
  2. 按验收判据检查关键日志是否出现（boot / camera / sd / wifi / ready / trigger / upload）
  3. 统计错误与警告，输出时间线

用法：
    python parse_log.py build_log_ov2640_optional.txt
    python parse_log.py serial.log --check            # 只跑判据检查
    python parse_log.py serial.log --tag CAM SD       # 只看这两个 TAG
    python parse_log.py serial.log --timeline > tl.txt
    python -m serial.tools.miniterm COM6 115200 | python parse_log.py -   # 管道
"""

import argparse
import re
import sys
from collections import Counter

LINE_RE = re.compile(
    r"^(?P<lvl>[IWEVD])\s*\((?P<t>\d+)\)\s*\[(?P<tag>[^\]]{1,8})\]\s*(?P<msg>.*)$"
)

# 任务书第 8 节验收判据：出现即视为该阶段通过
CHECKS = [
    ("启动横幅",        lambda r: r["tag"] == "BOOT" and "Snapshot Kit" in r["msg"]),
    ("芯片/PSRAM 信息", lambda r: r["tag"] == "BOOT" and "psram=" in r["msg"]),
    ("摄像头 init ok",  lambda r: r["tag"] == "CAM" and r["msg"].startswith("init ok")),
    ("SD 状态已确定",   lambda r: r["tag"] == "SD" and ("mount ok" in r["msg"] or
                                                        "degrade" in r["msg"] or
                                                        "disabled" in r["msg"])),
    ("Wi-Fi 有结果",    lambda r: r["tag"] == "WIFI" and ("sta connected" in r["msg"] or
                                                         "ap started" in r["msg"])),
    ("httpd 已启动",    lambda r: r["tag"] == "WEB" and "httpd started" in r["msg"]),
    ("mDNS 已注册",     lambda r: r["tag"] == "WEB" and "mdns" in r["msg"]),
    ("系统就绪",        lambda r: r["tag"] == "SYS" and "heap=" in r["msg"]),
]

FATAL_PATTERNS = [
    ("看门狗复位",      re.compile(r"Task watchdog got triggered|Guru Meditation", re.I)),
    ("堆耗尽",          re.compile(r"no mem|oom|MALLOC_FAIL|esp_malloc.*fail", re.I)),
    ("framebuffer 未归还", re.compile(r"fb.*leak|framebuffer.*not returned", re.I)),
    ("栈溢出",          re.compile(r"stack overflow|Stack canary", re.I)),
    ("abort/assert",    re.compile(r"abort\(\) was called|assert failed", re.I)),
]


def parse(fh):
    rows = []
    for raw in fh:
        line = raw.rstrip("\r\n")
        if not line.strip():
            continue
        m = LINE_RE.match(line.strip())
        if m:
            rows.append({
                "t_ms": int(m.group("t")),
                "level": m.group("lvl"),
                "tag": m.group("tag").strip(),
                "msg": m.group("msg"),
                "raw": line,
            })
        else:
            rows.append({"t_ms": None, "level": "?", "tag": "", "msg": line, "raw": line})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile", help="日志文件，或 - 表示 stdin")
    ap.add_argument("--check", action="store_true", help="只输出验收判据结果")
    ap.add_argument("--tag", nargs="*", default=None, help="只显示这些 TAG")
    ap.add_argument("--level", default="", help="只显示包含这些等级字母（如 WE）")
    ap.add_argument("--timeline", action="store_true", help="输出 秒  TAG  消息 的时间线")
    ap.add_argument("--grep", default="", help="只显示消息里包含该子串的行")
    args = ap.parse_args()

    fh = sys.stdin if args.logfile == "-" else open(args.logfile, "r", encoding="utf-8", errors="replace")
    with fh:
        rows = parse(fh)

    tagged = [r for r in rows if r["tag"]]
    print("共 %d 行，已识别结构化行 %d 行" % (len(rows), len(tagged)))

    # ---------------- 各等级 / TAG 统计 ----------------
    lv = Counter(r["level"] for r in tagged)
    print("等级分布: " + "  ".join("%s=%d" % (k, lv.get(k, 0)) for k in "IWE"))
    tg = Counter(r["tag"] for r in tagged)
    print("TAG 分布 : " + "  ".join("%s=%d" % (k, v) for k, v in tg.most_common()))

    # ---------------- 验收判据 ----------------
    print("\n---- 验收判据 ----")
    allok = True
    for name, pred in CHECKS:
        hit = next((r for r in tagged if pred(r)), None)
        if hit:
            print("  [PASS] %-16s t=%-7s %s" % (name, hit["t_ms"], hit["msg"][:80]))
        else:
            allok = False
            print("  [FAIL] %-16s 未找到" % name)

    # ---------------- 致命模式 ----------------
    print("\n---- 致命模式 ----")
    for name, rx in FATAL_PATTERNS:
        hits = [r for r in rows if rx.search(r["msg"])]
        if hits:
            allok = False
            print("  [HIT ] %-16s x%d  首条: %s" % (name, len(hits), hits[0]["msg"][:90]))
        else:
            print("  [none] %-16s" % name)

    if args.check:
        return 0 if allok else 1

    # ---------------- 过滤 + 时间线 ----------------
    sel = tagged
    if args.tag:
        want = {t.strip().upper() for t in args.tag}
        sel = [r for r in sel if r["tag"].upper() in want]
    if args.level:
        letters = set(args.level.upper())
        sel = [r for r in sel if r["level"] in letters]
    if args.grep:
        sel = [r for r in sel if args.grep in r["msg"]]

    print("\n---- 选中 %d 行 ----" % len(sel))
    for r in sel:
        if args.timeline:
            print("%8.3fs  %-5s %-6s %s" % (r["t_ms"] / 1000.0, r["level"], r["tag"], r["msg"]))
        else:
            print(r["raw"])

    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
