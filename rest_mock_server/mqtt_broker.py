#!/usr/bin/env python3
r"""一个够用的 MQTT 3.1.1 broker，纯 Python、零依赖、免安装、免管理员。

为什么要自己写：
  · mosquitto 的 Windows 包**只有 .exe 安装器**（NSIS，带 requireAdministrator
    清单），静默安装会直接撞 WinError 740，免不了管理员权限；
  · 本机没有 Node，npx aedes 那条路也不通；
  · 而这个工程对 broker 的需求极小 —— 设备**只发布不订阅**，我们只想
    把事件看下来。用一个几百行的 broker 完全够，还能顺手把收到的
    消息直接打印出来，省掉单独开订阅端。

支持（覆盖 esp-mqtt 客户端的实际用法）：
  · CONNECT / CONNACK（含 clean session、keepalive）
  · PUBLISH（QoS 0/1/2 收包，QoS1 回 PUBACK）
  · SUBSCRIBE / SUBACK（给 mqtt_watch.py 之类的订阅端用）
  · PINGREQ / PINGRESP
  · DISCONNECT
  · 主题通配符 + 和 #（订阅端用）

不支持（本场景用不到，遇到会明确说不支持而不是静默错）：
  · 认证（一律 allow，等价 allow_anonymous true）
  · TLS
  · 遗嘱消息（Last Will）—— 会忽略
  · retained 消息 —— 会忽略

用法:
    python mqtt_broker.py                    # 监听 0.0.0.0:1883
    python mqtt_broker.py --port 1884
    python mqtt_broker.py --host 127.0.0.1   # 只监听本机
    python mqtt_broker.py --quiet            # 不打印消息体
"""
import argparse
import socket
import struct
import sys
import threading
import time

# Windows 控制台默认是 GBK 代码页，本文件却是 UTF-8 带中文注释和日志。
# 一旦设备发来的 JSON 里含非 ASCII（或日志里的中文），print 会直接抛
# UnicodeEncodeError 把线程打断 —— 而这是**线程里抛的**，只会留一行
# traceback，看起来像"broker 自己崩了"。这里统一把 stdout/stderr 换成
# UTF-8 + errors=replace，杜绝这类噪音。
for _s in ("stdout", "stderr"):
    _f = getattr(sys, _s, None)
    if _f is not None and hasattr(_f, "reconfigure"):
        try:
            _f.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

# ---- MQTT 控制包类型 ----
CONNECT = 1
CONNACK = 2
PUBLISH = 3
PUBACK = 4
PUBREC = 5
PUBREL = 6
PUBCOMP = 7
SUBSCRIBE = 8
SUBACK = 9
UNSUBSCRIBE = 10
UNSUBACK = 11
PINGREQ = 12
PINGRESP = 13
DISCONNECT = 14

TYPE_NAME = {
    CONNECT: "CONNECT", CONNACK: "CONNACK", PUBLISH: "PUBLISH", PUBACK: "PUBACK",
    PUBREC: "PUBREC", PUBREL: "PUBREL", PUBCOMP: "PUBCOMP", SUBSCRIBE: "SUBSCRIBE",
    SUBACK: "SUBACK", UNSUBSCRIBE: "UNSUBSCRIBE", UNSUBACK: "UNSUBACK",
    PINGREQ: "PINGREQ", PINGRESP: "PINGRESP", DISCONNECT: "DISCONNECT",
}


def ts():
    return time.strftime("%H:%M:%S")


def log(msg):
    print(f"[{ts()}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# 变长整数（MQTT 特有编码，每字节低 7 位 + 延续位）
# ---------------------------------------------------------------------------
def enc_remaining_length(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            break
    return bytes(out)


def recv_exact(sock, n):
    """读满 n 字节。对端正常关闭返回 None。"""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_packet(sock):
    """读一个完整 MQTT 控制包，返回 (type, flags, payload) 或 None(断开)。

    fixed header 第 1 字节 = 类型(4bit) + flags(4bit)，之后是变长剩余长度。
    """
    head = recv_exact(sock, 1)
    if head is None:
        return None
    b0 = head[0]
    ptype = b0 >> 4
    flags = b0 & 0x0F

    # 剩余长度最多 4 字节
    multiplier = 1
    length = 0
    for _ in range(4):
        b = recv_exact(sock, 1)
        if b is None:
            return None
        length += (b[0] & 0x7F) * multiplier
        if not (b[0] & 0x80):
            break
        multiplier *= 128
    else:
        return None  # 超过 4 字节 = 协议错误

    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    return ptype, flags, payload


def make_packet(ptype, flags, payload):
    return bytes([(ptype << 4) | flags]) + enc_remaining_length(len(payload)) + payload


def topic_matches(filt, topic):
    """MQTT 主题匹配：+ 匹配一层，# 匹配剩余全部（只能在末尾）。"""
    if filt == topic:
        return True
    f = filt.split("/")
    t = topic.split("/")
    for i, seg in enumerate(f):
        if seg == "#":
            return True  # # 之后全匹配（含零层）
        if i >= len(t):
            return False
        if seg != "+" and seg != t[i]:
            return False
    return len(f) == len(t)


class Broker:
    def __init__(self, quiet=False):
        self.clients = {}          # client_id -> Client
        self.lock = threading.Lock()
        self.quiet = quiet
        self.pub_count = 0

    # ---- 广播给所有匹配的订阅者 ----
    def dispatch(self, topic, payload, qos):
        with self.lock:
            targets = [(c, s_qos) for c in self.clients.values()
                       for (f, s_qos) in c.subs.items() if topic_matches(f, topic)]
        for client, s_qos in targets:
            out_qos = min(qos, s_qos)
            client.send_publish(topic, payload, out_qos)

    def add(self, client):
        with self.lock:
            self.clients[id(client)] = client

    def remove(self, client):
        with self.lock:
            self.clients.pop(id(client), None)


class Client(threading.Thread):
    def __init__(self, sock, addr, broker):
        super().__init__(daemon=True)
        self.sock = sock
        self.addr = addr
        self.broker = broker
        self.client_id = "?"
        self.username = ""         # CONNECT 里带的用户名（我们不校验，仅记录）
        self.password_len = -1     # -1 = 对端没发密码；只记长度，不留明文
        self.subs = {}             # 订阅的 topic filter -> granted qos
        self.wlock = threading.Lock()
        self.packet_id = 0

    # ---- 带锁发送，避免多线程交叉写坏帧 ----
    def send(self, ptype, flags, payload):
        try:
            with self.wlock:
                self.sock.sendall(make_packet(ptype, flags, payload))
        except OSError:
            pass

    def send_publish(self, topic, payload, qos):
        t = topic.encode("utf-8")
        if qos == 0:
            body = struct.pack("!H", len(t)) + t + payload
            self.send(PUBLISH, 0, body)
        else:
            with self.wlock:
                self.packet_id = (self.packet_id % 65535) + 1
                pid = self.packet_id
            body = struct.pack("!H", len(t)) + t + struct.pack("!H", pid) + payload
            self.send(PUBLISH, 0x02, body)   # qos1 -> flags bit1

    def run(self):
        try:
            while True:
                pkt = recv_packet(self.sock)
                if pkt is None:
                    break
                if not self.handle(*pkt):
                    break
        except OSError:
            pass
        except Exception as e:
            # 兜底：单个畸形包不应该让线程抛出裸 traceback（既难读又掩盖了
            # 真正的协议问题）。明确报出客户端与异常，然后断开这一个连接。
            log(f"[警告] 处理 {self.addr[0]}:{self.addr[1]} 的包时异常: "
                f"{type(e).__name__}: {e} -> 断开该连接")
        finally:
            self.broker.remove(self)
            try:
                self.sock.close()
            except OSError:
                pass
            log(f"断开 {self.addr[0]}:{self.addr[1]} client_id={self.client_id} "
                f"（剩余连接数 {len(self.broker.clients)}）")

    def handle(self, ptype, flags, payload):
        if ptype == CONNECT:
            return self.on_connect(payload)
        if ptype == PUBLISH:
            return self.on_publish(flags, payload)
        if ptype == SUBSCRIBE:
            return self.on_subscribe(payload)
        if ptype == UNSUBSCRIBE:
            return self.on_unsubscribe(payload)
        if ptype == PINGREQ:
            self.send(PINGRESP, 0, b"")
            return True
        if ptype == DISCONNECT:
            log(f"DISCONNECT from client_id={self.client_id}")
            return False
        if ptype in (PUBACK, PUBREC, PUBREL, PUBCOMP):
            # QoS1/2 后续握手包：我们只做 QoS1 且不重传，收下即忽略
            return True
        log(f"[警告] 不支持的包类型 {TYPE_NAME.get(ptype, ptype)}，忽略")
        return True

    # ---------------- CONNECT ----------------
    def on_connect(self, payload):
        """按 MQTT 3.1.1 §3.1.3 的**可变头 + 负载**顺序解析。

        可变头: 协议名(UTF-8串) / 协议级别(1B) / 连接标志(1B) / keepalive(2B)
        负载  : ClientID / [Will Topic] / [Will Message] / [Username] / [Password]
                      ↑ 全部是"2 字节长度 + 内容"的 UTF-8/二进制串
        后三者的出现与否由连接标志的对应位决定。

        ★ 曾经踩过的坑：ClientID 原来被读在 keepalive 之后（等于读到了
          Username 的长度前缀）。匿名连接时恰好凑对，一旦设备填了
          username/password，指针从第一步就偏了，最后解 password 长度时
          越界 -> struct.error: unpack_from requires a buffer of at least
          N bytes。所以这里**不再靠偏移硬算**，改用统一的长度前缀读取器，
          每读一段都带边界检查。
        """
        try:
            pos, nlen = self._read_u16(payload, 0)
        except ValueError:
            log("[警告] CONNECT 包太短，无法读取协议名长度，丢弃")
            return False
        if pos + nlen > len(payload):
            log("[警告] CONNECT 包协议名字段越界，丢弃")
            return False
        proto = payload[pos:pos + nlen].decode("utf-8", "replace")
        pos += nlen

        if pos + 4 > len(payload):
            log("[警告] CONNECT 包在协议级别/标志/keepalive 处截断，丢弃")
            return False
        ver = payload[pos];       pos += 1
        cflags = payload[pos];    pos += 1
        (keepalive,) = struct.unpack_from("!H", payload, pos)
        pos += 2

        # ---- 负载：按规范顺序，逐段带长度前缀 ----
        def take_str(what):
            """读一个长度前缀串，返回 (新pos, 字符串)。越界 -> ValueError。"""
            nonlocal pos
            p, ln = self._read_u16(payload, pos)
            if p + ln > len(payload):
                raise ValueError(f"{what} 字段越界（声明 {ln}B，剩余 {len(payload) - p}B）")
            s = payload[p:p + ln]
            pos = p + ln
            return s

        try:
            cid = take_str("ClientID").decode("utf-8", "replace")
            self.client_id = cid or "(空)"

            # 协议版本：3=3.1, 4=3.1.1, 5=5.0。esp-mqtt 默认 3.1.1。
            # 只接受 3/4；5.0 的属性字段我们不解析，真要连会一直错位，
            # 与其错着跑不如明确拒绝。
            if ver not in (3, 4):
                log(f"[警告] 客户端 {self.client_id} 用的协议版本 {ver} 不支持"
                    f"（只支持 3.1/3.1.1），拒绝连接")
                self.send(CONNACK, 0, bytes([0x00, 0x01]))  # 0x01 = 版本不支持
                return False

            will = bool(cflags & 0x04)
            user = bool(cflags & 0x80)
            pw = bool(cflags & 0x40)

            # Will 是"主题串 + 消息串"两段（消息是二进制，同样 2B 长度前缀）
            if will:
                take_str("Will Topic")
                take_str("Will Message")
            if user:
                self.username = take_str("Username").decode("utf-8", "replace")
            if pw:
                # 密码不打印明文，只记长度，避免日志泄露
                self.password_len = len(take_str("Password"))
        except ValueError as e:
            # 走到这里说明对端发的不是标准 CONNECT（或截断）。明确报出来，
            # 不要让它变成线程里的裸 traceback。
            log(f"[警告] 解析 CONNECT 失败: {e}，丢弃该连接")
            return False

        self.broker.add(self)
        self.send(CONNACK, 0, bytes([0x00, 0x00]))  # 0x00 = accepted
        # 认证信息不落日志（只报"有没有"），符合 config_store 的密钥约定
        log(f"连接 {self.addr[0]}:{self.addr[1]} client_id={self.client_id} "
            f"{proto}{ver} keepalive={keepalive}s "
            f"user={'有' if user else '无'} pass={'有' if pw else '无'} "
            f"（连接数 {len(self.broker.clients)}）")
        return True

    @staticmethod
    def _read_u16(buf, pos):
        """读 2 字节大端长度前缀，返回 (内容起始偏移, 长度)。越界 -> ValueError。"""
        if pos + 2 > len(buf):
            raise ValueError(f"偏移 {pos} 处读长度前缀越界（包长 {len(buf)}）")
        (n,) = struct.unpack_from("!H", buf, pos)
        return pos + 2, n

    # ---------------- PUBLISH ----------------
    def on_publish(self, flags, payload):
        qos = (flags >> 1) & 0x03
        pos = 0
        (tlen,) = struct.unpack_from("!H", payload, pos)
        pos += 2
        topic = payload[pos:pos + tlen].decode("utf-8", "replace")
        pos += tlen

        pid = None
        if qos > 0:
            (pid,) = struct.unpack_from("!H", payload, pos)
            pos += 2

        body = payload[pos:]
        self.broker.pub_count += 1

        if not self.broker.quiet:
            text = body.decode("utf-8", "replace")
            log(f"PUBLISH <- {self.client_id}  topic={topic}  qos={qos} "
                f"{len(body)}B")
            log(f"          {text}")
        else:
            log(f"PUBLISH <- {self.client_id}  topic={topic}  qos={qos} {len(body)}B")

        if qos == 1 and pid is not None:
            self.send(PUBACK, 0, struct.pack("!H", pid))
        elif qos == 2 and pid is not None:
            # 完整 QoS2 要 PUBREC/PUBREL/PUBCOMP 三轮；设备用 QoS1，
            # 这里走 PUBREC 让流程不至于卡死。
            self.send(PUBREC, 0, struct.pack("!H", pid))

        self.broker.dispatch(topic, body, qos)
        return True

    # ---------------- SUBSCRIBE ----------------
    def on_subscribe(self, payload):
        (pid,) = struct.unpack_from("!H", payload, 0)
        pos = 2
        granted = bytearray()
        while pos < len(payload):
            (tlen,) = struct.unpack_from("!H", payload, pos)
            pos += 2
            filt = payload[pos:pos + tlen].decode("utf-8", "replace")
            pos += tlen
            req_qos = payload[pos] & 0x03
            pos += 1
            self.subs[filt] = req_qos
            granted.append(min(req_qos, 1))
            log(f"SUBSCRIBE {self.client_id} -> {filt} (qos {req_qos})")
        self.send(SUBACK, 0, struct.pack("!H", pid) + bytes(granted))
        return True

    def on_unsubscribe(self, payload):
        (pid,) = struct.unpack_from("!H", payload, 0)
        pos = 2
        while pos < len(payload):
            (tlen,) = struct.unpack_from("!H", payload, pos)
            pos += 2
            filt = payload[pos:pos + tlen].decode("utf-8", "replace")
            pos += tlen
            self.subs.pop(filt, None)
        self.send(UNSUBACK, 0, struct.pack("!H", pid))
        return True


def main():
    ap = argparse.ArgumentParser(description="纯 Python MQTT broker（免安装）")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址 (默认 0.0.0.0)")
    ap.add_argument("--port", type=int, default=1883, help="监听端口 (默认 1883)")
    ap.add_argument("--quiet", action="store_true", help="不打印消息体")
    args = ap.parse_args()

    broker = Broker(quiet=args.quiet)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((args.host, args.port))
    except OSError as e:
        print(f"error: 无法监听 {args.host}:{args.port} -> {e}", file=sys.stderr)
        if getattr(e, "winerror", None) == 10048:
            print("       端口已被占用（可能 broker 已经在跑）。", file=sys.stderr)
        return 1
    srv.listen(16)

    print("=" * 66)
    print(f"  MQTT broker (纯 Python) 监听 {args.host}:{args.port}")
    print("  设备侧 main/app_conf.h 应填:")
    print(f"      APP_MQTT_HOST  <你的局域网IP>")
    print(f"      APP_MQTT_PORT  {args.port}")
    print("  订阅端:  python mqtt_watch.py --port %d" % args.port)
    print("  停止:    Ctrl+C")
    print("=" * 66)
    print()

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            Client(conn, addr, broker).start()
    except KeyboardInterrupt:
        print()
        log(f"停止。累计收到 {broker.pub_count} 条 PUBLISH。")
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
