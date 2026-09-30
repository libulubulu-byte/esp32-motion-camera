#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-S3 Snapshot Kit 本地接收服务器（用于验证 HTTP 上传链路）

严格匹配固件 main/upload_http.c 的协议：
  POST <http_url 原样路径>
  Content-Type: multipart/form-data; boundary=----ESP32S3SnapKitBoundary
    |- name="meta"  Content-Type: application/json   <- 事件元数据
    '- name="file"  filename="snapshot.jpg"           <- JPEG 原图
  可选自定义头：X-Device-Key: <http_header_value>（值为空时设备不发）
  返回 200/201/202 才算成功，否则设备重试 3 次（退避 1/2/4s，超时 10s）

依赖：仅 Python 3 标准库。
  ★ 刻意不用 cgi.FieldStorage —— 该模块在 Python 3.13 已被移除，
    所以这里手写 multipart 解析，保证在任何 3.x 上都能跑。

用法：
  python recv_server.py                    # 监听 0.0.0.0:8080
  python recv_server.py 9000               # 自定义端口
  python recv_server.py --check-key s3cr3t # 校验 X-Device-Key，不匹配回 401
  python recv_server.py --dir D:\\shots     # 自定义保存目录

设备侧配置（网页或 POST /api/config）：
  http_url     = http://<本机局域网IP>:8080/upload
  upload_mode  = http（或 both）
"""

import argparse
import datetime
import json
import os
import re
import sys
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 固件里硬编码的 boundary（upload_http.c: BOUND），仅作 Content-Type 缺失时的兜底
DEFAULT_BOUNDARY = b"----ESP32S3SnapKitBoundary"
MULTIPART_MAX = 8 * 1024 * 1024   # JPEG 最大 1.5MB，留足余量防滥用


def extract_boundary(ctype):
    """优先用 Content-Type 声明的 boundary，这样 curl 自测也能解析。"""
    m = re.search(r'boundary="?([^";]+)"?', ctype, re.I)
    return m.group(1).encode() if m else DEFAULT_BOUNDARY


def parse_multipart(body, boundary):
    """把 multipart body 拆成 {字段名: 原始字节}。手写实现，兼容 Python 3.13+。"""
    fields = {}
    for chunk in body.split(b"--" + boundary):
        # 首块为空；末块是收尾分隔符 "--\r\n"，以 "--" 开头，跳过
        if not chunk or chunk.startswith(b"--"):
            continue
        if chunk.startswith(b"\r\n"):
            chunk = chunk[2:]
        if chunk.endswith(b"\r\n"):
            chunk = chunk[:-2]
        head, sep, data = chunk.partition(b"\r\n\r\n")
        if not sep:
            continue
        name = None
        for line in head.decode("utf-8", "replace").split("\r\n"):
            if line.lower().startswith("content-disposition:"):
                m = re.search(r'name="([^"]+)"', line)
                if m:
                    name = m.group(1)
        if name:
            fields[name] = data
    return fields


class RecvHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # 由 main() 注入
    save_dir = "received"
    check_key = None
    stats = {"ok": 0, "bad": 0}

    # ------------------------------------------------------------------ 工具
    def _reply(self, code, payload, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *a):
        """屏蔽默认的每请求一行噪音，我们自己的输出更详细。"""
        pass

    # ------------------------------------------------------------------ 路由
    def do_GET(self):
        """浏览器打开即可确认服务活着（配设备前先用这步排除防火墙问题）。"""
        self._reply(200, b'{"server":"alive","hint":"POST multipart to this URL"}\n')

    def do_POST(self):
        """薄封装：**任何**未捕获异常都必须回一个响应。

        ★ 这是踩过的坑：若 handler 抛异常，socketserver 只会打印 traceback 并
          关掉连接，设备端 esp_http_client 拿到的是"无应答"，于是判定失败并按
          1/2/4s 退避重试 3 次 —— 从串口/设备侧完全看不出是服务器崩了。
          历史 Bug：os.path.relpath(jpg_path) 未传 start，CWD 在 C 盘而
          save_dir 在 D 盘时跨盘符抛 ValueError，导致每次上传都不回包。
        """
        try:
            self._handle_post()
        except Exception:
            self.stats["bad"] += 1
            print("  !! 服务器内部错误 -> 500（设备会重试 3 次）")
            traceback.print_exc()
            try:
                self._reply(500, b'{"error":"internal"}\n')
            except Exception:
                pass

    def _handle_post(self):
        stamp = datetime.datetime.now()
        n = int(self.headers.get("Content-Length") or 0)
        ctype = self.headers.get("Content-Type", "")
        devkey = self.headers.get("X-Device-Key", "")

        print("\n" + "=" * 70)
        print(f"[{stamp:%H:%M:%S}] POST {self.path}")
        print(f"  Content-Type : {ctype}")
        print(f"  Content-Len  : {n}")
        if devkey:
            print(f"  X-Device-Key : {devkey}")

        # --- admin key 校验（可选）---
        if self.check_key is not None and devkey != self.check_key:
            self.stats["bad"] += 1
            print("  !! X-Device-Key 不匹配 -> 401（设备会重试 3 次后放弃）")
            return self._reply(401, b'{"error":"unauthorized"}\n')

        if not n:
            self.stats["bad"] += 1
            print("  !! body 为空 -> 400")
            return self._reply(400, b'{"error":"empty body"}\n')

        if n > MULTIPART_MAX:
            self.stats["bad"] += 1
            print(f"  !! body 过大 {n} > {MULTIPART_MAX} -> 413")
            return self._reply(413, b'{"error":"too large"}\n')

        body = self.rfile.read(n)

        if "multipart/form-data" not in ctype.lower():
            self.stats["bad"] += 1
            print(f"  !! 不是 multipart，前 200 字节: {body[:200]!r} -> 400")
            return self._reply(400, b'{"error":"not multipart"}\n')

        fields = parse_multipart(body, extract_boundary(ctype))
        print(f"  解析字段     : {list(fields.keys())}")

        # --- meta ---
        meta = None
        meta_raw = fields.get("meta")
        if meta_raw:
            try:
                meta = json.loads(meta_raw.decode("utf-8", "replace"))
                print("  meta         : " + json.dumps(meta, ensure_ascii=False))
            except Exception as e:
                print(f"  !! meta 解析失败 {e}: {meta_raw[:200]!r}")
                print("  （多半是 boundary 不匹配）")
        else:
            print("  !! 缺少 meta 字段")

        # --- JPEG ---
        jpg = fields.get("file")
        if jpg:
            self.stats["ok"] += 1
            eid = (meta or {}).get("event_id", self.stats["ok"])
            tag = stamp.strftime("%Y%m%d_%H%M%S")
            base = f"{tag}_ev{eid}"
            jpg_path = os.path.join(self.save_dir, base + ".jpg")
            with open(jpg_path, "wb") as f:
                f.write(jpg)

            soif = jpg[:2] == b"\xff\xd8"
            eoif = jpg[-2:] == b"\xff\xd9"
            # ★ 打印绝对路径，不要用 os.path.relpath(p)（单参形式会以 CWD 为基准，
            #   CWD 与 save_dir 跨盘符时抛 ValueError）。
            print(f"  JPEG         : {len(jpg)} 字节 -> {jpg_path}")
            print(f"  SOI/EOI 标记 : " +
                  ("OK" if soif else "缺失!") + " / " + ("OK" if eoif else "缺失!"))
            if not (soif and eoif):
                print("  !! JPEG 头尾标记异常 —— 可能是坏帧（对照串口 NO-SOI / EV-EOF-OVF）")

            if meta:
                with open(os.path.join(self.save_dir, base + ".json"),
                          "w", encoding="utf-8") as f:
                    json.dump(meta, f, ensure_ascii=False, indent=2)
        else:
            print("  !! 缺少 file 字段")

        total = self.stats["ok"] + self.stats["bad"]
        print(f"  -> 回 200 OK（设备判定成功）  累计 成功={self.stats['ok']} "
              f"异常={self.stats['bad']} 总={total}")
        return self._reply(200, b'{"ok":true}\n')


def main():
    ap = argparse.ArgumentParser(
        description="ESP32-S3 Snapshot Kit 本地上传接收服务器",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("port", nargs="?", type=int, default=8080, help="监听端口")
    ap.add_argument("--dir", default=None,
                    help="JPEG/JSON 保存目录（默认脚本同级的 received/）")
    ap.add_argument("--check-key", default=None, metavar="KEY",
                    help="若指定，则要求 X-Device-Key 等于该值，否则回 401")
    ap.add_argument("--bind", default="0.0.0.0", help="监听地址")
    args = ap.parse_args()

    base = os.path.dirname(os.path.abspath(__file__))
    save_dir = args.dir or os.path.join(base, "received")
    os.makedirs(save_dir, exist_ok=True)

    RecvHandler.save_dir = save_dir
    RecvHandler.check_key = args.check_key

    print("ESP32-S3 Snapshot Kit 本地接收服务器")
    print(f"  监听地址 : {args.bind}:{args.port}")
    print(f"  保存目录 : {save_dir}")
    if args.check_key is not None:
        print(f"  Key 校验 : 已开启（须匹配 X-Device-Key）")
    else:
        print(f"  Key 校验 : 关闭（仅打印收到的 X-Device-Key）")
    print(f"  设备请填 : http://<本机局域网IP>:{args.port}/upload")
    print("  查本机 IP: ipconfig -> Wi-Fi 适配器的 IPv4 地址（勿用 127.0.0.1）")
    print("  自检     : 浏览器打开 http://<本机IP>:%d/ 应返回 server=alive" % args.port)
    print("  注意     : 首次运行请允许 Windows 防火墙「专用网络」入站")
    print("  Ctrl+C 退出\n")

    srv = ThreadingHTTPServer((args.bind, args.port), RecvHandler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
