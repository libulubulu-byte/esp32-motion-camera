#!/usr/bin/env python3
r"""订阅 ESP32-S3 Snapshot Kit 的 MQTT 事件，实时打印到终端。

设备侧（main/mqtt_hal.c）只 **发布**，从不订阅，所以本脚本是纯观察者：
它不回复任何消息，只把设备上报的 JSON 打印出来（顺便解析成一行摘要）。

主题格式（main/mqtt_hal.c:67）：
    <APP_MQTT_TOPIC_PREFIX>/<MAC 大写十六进制无分隔>/event
    例: esp32S3_CAM/288485922F44/event

用法（在 rest_mock_server 目录下）：
    python mqtt_watch.py
    python mqtt_watch.py --host 192.168.0.101 --port 1883
    python mqtt_watch.py --topic 'esp32S3_CAM/+/event'      # 看全部设备
    python mqtt_watch.py --raw                              # 不解析，原样打印
    python mqtt_watch.py --user u --password p

默认订阅 `esp32S3_CAM/+/event`，即"所有设备的事件" —— 比钉死某个 MAC 更实用，
换板子、换 MAC 都不用改参数。
"""
import argparse
import json
import signal
import sys
import time

# Windows 控制台默认 GBK，而设备上报的 JSON / 本脚本的中文日志都是 UTF-8。
# 不换成 UTF-8 的话，遇到中文或非法字节会抛 UnicodeEncodeError 打断接收循环。
for _s in ("stdout", "stderr"):
    _f = getattr(sys, _s, None)
    if _f is not None and hasattr(_f, "reconfigure"):
        try:
            _f.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("error: 缺少 paho-mqtt，先执行:", file=sys.stderr)
    print("       python -m pip install paho-mqtt", file=sys.stderr)
    sys.exit(1)

# 与 main/app_conf.h 的 APP_MQTT_TOPIC_PREFIX 保持一致
DEFAULT_PREFIX = "esp32S3_CAM"


def parse_args():
    ap = argparse.ArgumentParser(
        description="订阅并打印 ESP32-S3 Snapshot Kit 的 MQTT 事件"
    )
    ap.add_argument("--host", default="127.0.0.1", help="broker 地址 (默认 127.0.0.1)")
    ap.add_argument("--port", type=int, default=1883, help="broker 端口 (默认 1883)")
    ap.add_argument("--topic", default=f"{DEFAULT_PREFIX}/+/event",
                    help=f"订阅主题 (默认 {DEFAULT_PREFIX}/+/event)")
    ap.add_argument("--qos", type=int, default=0, choices=(0, 1, 2), help="QoS")
    ap.add_argument("--user", default=None, help="用户名 (broker 要求认证时)")
    ap.add_argument("--password", default=None, help="密码")
    ap.add_argument("--raw", action="store_true", help="不解析 JSON，原样打印")
    return ap.parse_args()


def ts():
    return time.strftime("%H:%M:%S")


def summarize(payload):
    """把设备的事件 JSON 压成一行摘要。

    字段名取自 upload_queue.c 组装的 meta。解析失败就返回 None，调用方
    会退回打印原文 —— 宁可格式难看，也不要漏掉消息。
    """
    try:
        d = json.loads(payload)
    except Exception:
        return None
    if not isinstance(d, dict):
        return None

    parts = []
    for key, label in (("device", "dev"), ("version", "ver"),
                       ("event", "evt"), ("reason", "reason"),
                       ("rssi", "rssi"), ("size", "size"),
                       ("id", "id"), ("trigger", "trig")):
        if key in d and d[key] not in (None, ""):
            parts.append(f"{label}={d[key]}")
    # 兜底：一个已知字段都没有，就列出全部键值
    if not parts:
        parts = [f"{k}={v}" for k, v in list(d.items())[:6]]
    return " ".join(str(p) for p in parts)


def main():
    args = parse_args()

    # paho-mqtt 2.x 必须显式给 CallbackAPIVersion，且 VERSION1 已标记 deprecated
    # （会打 DeprecationWarning）。这里优先按 2.x 的新式 VERSION2 建客户端，
    # 拿不到（1.x 没有 CallbackAPIVersion）再退回旧签名 —— 三个版本都能跑。
    cid = f"snapkit-watch-{int(time.time()) % 100000}"
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
        _api = 2
    except AttributeError:
        try:
            client = mqtt.Client(client_id=cid)
            _api = 1
        except Exception as e:
            print(f"error: 创建 MQTT 客户端失败: {e}", file=sys.stderr)
            return 1

    if args.user:
        client.username_pw_set(args.user, args.password)

    state = {"count": 0, "connected": False}

    def on_connect(c, userdata, flags, rc, properties=None):
        # VERSION2 回调的 rc 是 ReasonCode 对象，不是 int：
        # 它没有 __eq__，rc != 0 恒为 True，会把"连接成功"也判成失败。
        # 所以统一在这里转成 int 再用。
        code = int(getattr(rc, "value", rc))
        if code == 0:
            state["connected"] = True
            print(f"[{ts()}] 已连接 {args.host}:{args.port}，订阅 {args.topic}")
            c.subscribe(args.topic, qos=args.qos)
            print(f"[{ts()}] 等待设备上报... (Ctrl+C 退出)")
            print("-" * 70)
        else:
            # code 非 0 的常见原因：1=协议版本不对 2=client_id 被拒
            # 4=用户名密码错 5=未授权(常见于 allow_anonymous false)
            print(f"[{ts()}] 连接被拒绝 rc={code}", file=sys.stderr)
            if code == 135:
                # 实测踩过的坑：1883 上跑着 D:\Mosquitto\mosquitto.exe 时，
                # paho 报的是 135（CONNACK 前对端就把 TCP 关了），而不是 5。
                # mosquitto 默认 allow_anonymous false + 无 password_file，
                # 匿名连接在握手阶段就被掐。别被 "135" 这个数字误导成协议错误。
                print("        提示: 对端在握手阶段关闭了连接（rc=135，不是鉴权失败 rc=5）。",
                      file=sys.stderr)
                print("          多半是 mosquitto 占着 1883。两种做法：", file=sys.stderr)
                print("            A) 停掉它，用本仓库 broker:  taskkill /PID <pid> /F"
                      "  然后重跑 mqtt_start.bat", file=sys.stderr)
                print("            B) 给 mosquitto.conf 加 allow_anonymous true 后重启",
                      file=sys.stderr)
                print("          查 PID:  netstat -ano | findstr :1883", file=sys.stderr)
            if code == 5:
                # 实测踩过的坑：1883 上跑着的是系统装的 mosquitto（��认
                # allow_anonymous false + 无 password_file），匿名连接必被拒。
                # 本仓库的 mqtt_broker.py 完全不鉴权，绝不会回 rc=5。
                print("        提示: broker 拒绝了匿名连接。两种可能：", file=sys.stderr)
                print("          1) 1883 上是 mosquitto 而非本仓库 broker —— 先确认：",
                      file=sys.stderr)
                print("             netstat -ano | findstr :1883   再看该 PID 的进程名",
                      file=sys.stderr)
                print("             是 mosquitto.exe 就停掉它（taskkill /PID <pid> /F），"
                      "再跑 mqtt_start.bat", file=sys.stderr)
                print("          2) broker 确实要认证 —— 加 --user/--password",
                      file=sys.stderr)

    # ★ 签名必须是 5 参：VERSION2 的回调是
    #   (client, userdata, disconnect_flags, reason_code, properties)
    # 少写一个 disconnect_flags 时，paho 传 5 个实参进 4 参函数，直接抛
    #   TypeError: on_disconnect() takes from 3 to 4 positional arguments
    # 而且这个异常发生在 loop_read 内部，会把整个订阅端打成 traceback 退出。
    # disconnect_flags 这里用不到，但**位置不能省**，否则 reason_code 会错位。
    def on_disconnect(c, userdata, disconnect_flags, rc, properties=None):
        state["connected"] = False
        # 同 on_connect：VERSION2 下这里也是 ReasonCode，先转 int 再比。
        code = int(getattr(rc, "value", rc))
        if code != 0:
            # 非主动断开由 paho 的 loop 自动重连，这里只提示一下
            # rc=135 (0x87) 就是本例：broker 没起来 / 被拒，见下方 on_connect 提示
            print(f"[{ts()}] 连接断开 rc={code}，等待自动重连...", file=sys.stderr)

    def on_message(c, userdata, msg):
        state["count"] += 1
        payload = msg.payload.decode("utf-8", errors="replace")
        topic = msg.topic
        line = None if args.raw else summarize(payload)
        if line:
            print(f"[{ts()}] #{state['count']:<4} {topic}")
            print(f"          {line}")
        else:
            print(f"[{ts()}] #{state['count']:<4} {topic}  ({len(msg.payload)} bytes)")
            print(f"          {payload}")

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    def on_sigint(signum, frame):
        print(f"\n[{ts()}] 共收到 {state['count']} 条消息，退出。")
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sigint)

    try:
        client.connect(args.host, args.port, keepalive=30)
    except Exception as e:
        print(f"[{ts()}] 连不上 {args.host}:{args.port} -> {e}", file=sys.stderr)
        print("        确认 broker 在跑:  mqtt_start.bat", file=sys.stderr)
        return 1

    # loop_forever 内部会自己重连，不要自己再套一层 while
    client.loop_forever()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
