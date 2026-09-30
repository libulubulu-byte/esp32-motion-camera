#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时调试工具：从 PC 直接访问 ESP32（绑定源接口，绕开 TUN 代理劫持）。

用法：
    python _devreq.py /api/status
    python _devreq.py /api/log
    python _devreq.py POST /api/snapshot
    python _devreq.py POSTRAW /api/detect_raw face.jpg
"""
import os
import socket
import sys

DEV = os.environ.get("DEV_IP", "192.168.137.128")
SRC = "192.168.137.1"


def request(method, path, body=b"", ctype="application/octet-stream",
            host=DEV, src=SRC, timeout=30):
    s = socket.socket()
    s.bind((src, 0))
    s.settimeout(timeout)
    s.connect((host, 80))
    head = (f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n"
            f"Connection: close\r\n")
    if method == "POST":
        head += f"Content-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
    head += "\r\n"
    s.sendall(head.encode() + body)
    buf = b""
    while True:
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
    s.close()
    resp_head, _, resp_body = buf.partition(b"\r\n\r\n")
    return resp_head.decode("latin1"), resp_body


def main():
    args = sys.argv[1:]
    if not args:
        args = ["GET", "/api/status"]
    method = args[0].upper()
    if args[0].startswith("/"):
        # 只给了路径：默认 GET（避免把路径当成方法名而请求成大写路径）
        h, b = request("GET", args[0])
    elif method in ("GET", "POST"):
        path = args[1] if len(args) > 1 else "/api/status"
        body = b""
        if method == "POST" and len(args) > 2:
            with open(args[2], "rb") as f:
                body = f.read()
            ctype = "image/jpeg"
        else:
            ctype = "application/json"
        h, b = request(method, path, body, ctype)
    elif method == "POSTRAW":
        path = args[1]
        with open(args[2], "rb") as f:
            body = f.read()
        h, b = request("POST", path, body, "image/jpeg")
    else:
        path = method
        h, b = request("GET", path)
    print(h.splitlines()[0] if h else "(no response)")
    sys.stdout.write(b.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
