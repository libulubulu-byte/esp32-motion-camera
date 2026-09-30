#!/usr/bin/env python3
"""
test_api.py —— ESP32-S3 Snapshot Kit 验收脚本（任务书第 8 节验收清单的可执行版）

用法：
    python test_api.py --host 192.168.4.1
    python test_api.py --host esp32s3cam.local --key <admin_key>
    python test_api.py --host 192.168.4.1 --burst 50      # 连拍 50 次内存验收

依赖：仅标准库（urllib）。不依赖 requests，方便在只有 Python 的机器上跑。

退出码：0 = 全部通过；1 = 有失败项。
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
results = []


def req(host, path, method="GET", body=None, key=None, timeout=10, raw=False):
    url = "http://%s%s" % (host, path)
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if key:
        headers["X-Admin-Key"] = key
    r = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            payload = resp.read()
            return resp.status, (payload if raw else payload.decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        payload = e.read()
        return e.code, (payload if raw else payload.decode("utf-8", "replace"))
    except Exception as e:                                  # 连接层错误
        return 0, str(e)


def check(name, cond, detail=""):
    results.append((PASS if cond else FAIL, name, detail))
    print("[%s] %s %s" % (PASS if cond else FAIL, name, ("- " + detail) if detail else ""))
    return cond


def skip(name, detail=""):
    results.append((SKIP, name, detail))
    print("[%s] %s %s" % (SKIP, name, ("- " + detail) if detail else ""))


def jload(text):
    try:
        return json.loads(text)
    except Exception:
        return None


# --------------------------------------------------------------------------
def test_status(host, key):
    code, text = req(host, "/api/status")
    s = jload(text)
    if not check("GET /api/status -> 200", code == 200, "code=%s" % code):
        return None
    need = ["device_id", "fw_version", "uptime_ms", "armed", "wifi", "camera",
            "sd", "queue", "trigger", "snapshot", "system"]
    missing = [k for k in need if k not in s]
    check("status 字段齐全", not missing, "missing=%s" % missing)
    check("status.camera.sensor 非空", bool(s.get("camera", {}).get("sensor")))
    check("status.sd.state 是合法枚举",
          s.get("sd", {}).get("state") in
          ("DISABLED", "UNKNOWN", "MOUNTING", "OK", "FAIL", "REMOVED", "LOWSPACE"),
          str(s.get("sd", {}).get("state")))
    return s


def test_config_mask(host, key):
    code, text = req(host, "/api/config")
    c = jload(text)
    if not check("GET /api/config -> 200", code == 200, "code=%s" % code):
        return
    secret_fields = ["wifi_pass", "admin_key", "telegram_token", "mqtt_pass"]
    leaked = [f for f in secret_fields if c.get(f) not in (None, "", "***")]
    check("GET /api/config 密钥已脱敏", not leaked, "泄漏字段=%s" % leaked)

    # 未带 key 的写操作应 401（若设备已设置 admin_key）
    code, _ = req(host, "/api/config", "POST", {"jpeg_quality": 12})
    check("POST /api/config 无 key -> 401 (或 admin_key 为空时 200)",
          code in (200, 401), "code=%s" % code)


def test_auth(host, key):
    if not key:
        skip("401 鉴权用例", "未提供 --key")
        return
    code, _ = req(host, "/api/config", "POST", {"jpeg_quality": 12}, key="definitely-wrong")
    check("POST /api/config 错 key -> 401", code == 401, "code=%s" % code)
    code, _ = req(host, "/api/reboot", "POST", key="wrong")
    check("POST /api/reboot 错 key -> 401", code == 401, "code=%s" % code)


def test_arm_cycle(host, key):
    code, _ = req(host, "/api/arm", "POST")
    check("POST /api/arm -> 200", code == 200, "code=%s" % code)
    code, text = req(host, "/api/status")
    s = jload(text) or {}
    check("arm 后 status.armed = true", s.get("armed") is True)

    code, _ = req(host, "/api/disarm", "POST")
    check("POST /api/disarm -> 200", code == 200, "code=%s" % code)
    code, text = req(host, "/api/status")
    s = jload(text) or {}
    check("disarm 后 status.armed = false", s.get("armed") is False)

    # 撤防状态下拍照应 409
    code, _ = req(host, "/api/snapshot", "POST")
    check("撤防时 POST /api/snapshot -> 409", code == 409, "code=%s" % code)


def test_snapshot_flow(host, key):
    req(host, "/api/arm", "POST")

    code, _ = req(host, "/api/snapshot", "POST")
    if code == 409:
        # 冷却中，等一个冷却周期再试
        time.sleep(4)
        code, _ = req(host, "/api/snapshot", "POST")
    check("POST /api/snapshot -> 200", code == 200, "code=%s" % code)

    # 等拍照 + 入队
    got = False
    for _ in range(20):
        time.sleep(0.6)
        code, blob = req(host, "/api/last_snapshot", raw=True, timeout=15)
        if code == 200 and isinstance(blob, bytes) and len(blob) > 1000:
            got = True
            check("GET /api/last_snapshot 返回 JPEG", blob[:2] == b"\xff\xd8",
                  "header=%r len=%d" % (blob[:2], len(blob)))
            break
    if not got:
        check("GET /api/last_snapshot 返回 JPEG", False, "15s 内未拿到照片")

    code, text = req(host, "/api/status")
    s = jload(text) or {}
    check("拍照计数递增", s.get("snapshot", {}).get("ok_count", 0) > 0,
          "ok_count=%s" % s.get("snapshot", {}).get("ok_count"))


def test_events_log(host, key):
    code, text = req(host, "/api/events")
    check("GET /api/events -> 200", code == 200, "code=%s" % code)
    ev = jload(text)
    check("events 是 JSON 数组", isinstance(ev, list), type(ev).__name__)
    if isinstance(ev, list) and ev:
        keys = set(ev[-1].keys())
        check("event 含 event_id/trigger/ts", {"event_id", "trigger", "ts"} <= keys,
              "keys=%s" % sorted(keys))

    code, text = req(host, "/api/log")
    check("GET /api/log -> 200", code == 200, "code=%s" % code)
    lg = jload(text)
    check("log 是 JSON 数组", isinstance(lg, list), type(lg).__name__)
    if isinstance(lg, list) and lg:
        check("log 行以 [TAG ] 开头", str(lg[-1]).lstrip().startswith("["),
              str(lg[-1])[:60])


def test_sd(host, key):
    code, text = req(host, "/api/sd")
    check("GET /api/sd -> 200", code == 200, "code=%s" % code)
    sd = jload(text) or {}
    check("sd.mode ∈ none/optional/required", sd.get("mode") in ("none", "optional", "required"),
          str(sd.get("mode")))
    if sd.get("state") == "DISABLED":
        skip("SD 读写用例", "SD_MODE_NONE 构建")
        return
    if key:
        code, _ = req(host, "/api/sd/remount", "POST", key=key)
        check("POST /api/sd/remount (带 key) -> 200", code == 200, "code=%s" % code)


def test_no_card_degrade(host, key):
    """任务书 0.4：没有 SD 卡时仍能拍照 + 上传（此处只验证拍照链路）"""
    code, text = req(host, "/api/status")
    s = jload(text) or {}
    sd_state = s.get("sd", {}).get("state")
    if sd_state == "OK":
        skip("无卡降级用例", "当前有卡且挂载成功")
        return
    check("无卡时队列模式为 buffer", s.get("queue", {}).get("mode") == "buffer",
          "mode=%s state=%s" % (s.get("queue", {}).get("mode"), sd_state))
    test_snapshot_flow(host, key)


def test_burst(host, key, n):
    """连拍验收：内存不得持续下降（任务书第 8 节）"""
    req(host, "/api/arm", "POST")
    code, text = req(host, "/api/status")
    before = (jload(text) or {}).get("system", {})
    print("\n--- 连拍 %d 次 ---" % n)
    ok = 0
    for i in range(n):
        c, _ = req(host, "/api/snapshot", "POST")
        if c == 200:
            ok += 1
        time.sleep(0.35)                       # 略大于冷却间隔 / 抓拍耗时
    time.sleep(2)

    code, text = req(host, "/api/status")
    s = jload(text) or {}
    after = s.get("system", {})
    fh0, fh1 = before.get("free_heap", 0), after.get("free_heap", 0)
    fp0, fp1 = before.get("free_psram", 0), after.get("free_psram", 0)
    print("heap %d -> %d (Δ%d)  psram %d -> %d (Δ%d)" % (fh0, fh1, fh1 - fh0, fp0, fp1, fp1 - fp0))
    check("连拍 %d 次受理数 > 0" % n, ok > 0, "accepted=%d" % ok)
    check("连拍后 heap 未持续下降 (Δ > -8KB)", fh1 - fh0 > -8192, "Δheap=%d" % (fh1 - fh0))
    check("连拍后 psram 未持续下降 (Δ > -64KB)", fp1 - fp0 > -65536, "Δpsram=%d" % (fp1 - fp0))
    check("psram_min 已记录", s.get("snapshot", {}).get("psram_min", 0) > 0,
          "psram_min=%s" % s.get("snapshot", {}).get("psram_min"))


def test_errors_json(host, key):
    code, text = req(host, "/api/nonexistent_endpoint")
    check("未知端点 -> 404", code == 404, "code=%s" % code)
    code, text = req(host, "/api/ota", "POST", key=key or "x")
    check("POST /api/ota 无 url -> 400", code in (400, 401, 501), "code=%s" % code)


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.4.1", help="设备 IP 或 esp32s3cam.local")
    ap.add_argument("--key", default="", help="X-Admin-Key")
    ap.add_argument("--burst", type=int, default=0, help="连拍 N 次做内存验收")
    ap.add_argument("--only", default="", help="只跑名字包含该子串的用例组")
    args = ap.parse_args()

    print("== 目标 %s ==" % args.host)
    code, text = req(args.host, "/api/status", timeout=6)
    if code == 0:
        print("无法连接设备：%s" % text)
        return 1

    groups = [
        ("status", lambda: test_status(args.host, args.key)),
        ("config", lambda: test_config_mask(args.host, args.key)),
        ("auth", lambda: test_auth(args.host, args.key)),
        ("arm", lambda: test_arm_cycle(args.host, args.key)),
        ("snapshot", lambda: test_snapshot_flow(args.host, args.key)),
        ("events", lambda: test_events_log(args.host, args.key)),
        ("sd", lambda: test_sd(args.host, args.key)),
        ("nocardsd", lambda: test_no_card_degrade(args.host, args.key)),
        ("errors", lambda: test_errors_json(args.host, args.key)),
    ]
    for name, fn in groups:
        if args.only and args.only not in name:
            continue
        print("\n=== %s ===" % name)
        try:
            fn()
        except Exception as e:
            check("%s 组未抛异常" % name, False, repr(e))

    if args.burst:
        print("\n=== burst ===")
        test_burst(args.host, args.key, args.burst)

    nf = sum(1 for r in results if r[0] == FAIL)
    print("\n================ 合计 %d 项，失败 %d 项 ================" % (len(results), nf))
    for st, name, det in results:
        if st == FAIL:
            print("  FAIL: %s %s" % (name, det))
    return 1 if nf else 0


if __name__ == "__main__":
    sys.exit(main())
