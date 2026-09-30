#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时调试版中继：在原 telegram_relay.py 基础上加双向字节计数，用于定位 TLS 卡在哪。
用法同原脚本： python _relay_dbg.py [--proxy 127.0.0.1:7892]
"""
import argparse
import socket
import threading
import time

UPSTREAM_HOST = "api.telegram.org"
UPSTREAM_PORT = 443
FALLBACK_IPS = ["149.154.166.110", "149.154.167.220", "149.154.175.50"]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] [dbg] {msg}", flush=True)


def upstream_ips():
    ips = []
    try:
        for ai in socket.getaddrinfo(UPSTREAM_HOST, UPSTREAM_PORT, socket.AF_INET):
            ip = ai[4][0]
            if ip.startswith("192.168.137.") or ip.startswith("127."):
                continue
            if ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    for ip in FALLBACK_IPS:
        if ip not in ips:
            ips.append(ip)
    return ips


def open_upstream(proxy):
    if proxy:
        try:
            s = socket.create_connection(proxy, timeout=8)
            # ★ 关键：CONNECT 完成后必须去掉超时，否则 8s 无数据就会掐断 TLS 会话
            s.settimeout(None)
            req = (f"CONNECT {UPSTREAM_HOST}:{UPSTREAM_PORT} HTTP/1.1\r\n"
                   f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}\r\n\r\n").encode()
            s.sendall(req)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = s.recv(256)
                if not d:
                    raise OSError("proxy closed early")
                buf += d
            status = buf.split(b"\r\n", 1)[0].decode("latin1", "replace")
            if " 200" not in status:
                raise OSError(f"CONNECT failed: {status}")
            log(f"upstream ready (via proxy {proxy[0]}:{proxy[1]})")
            return s
        except OSError as e:
            log(f"proxy path failed ({e}) -> direct")
    for ip in upstream_ips():
        try:
            s = socket.create_connection((ip, UPSTREAM_PORT), timeout=8)
            s.settimeout(None)   # 同上：连上后必须阻塞等待
            log(f"upstream ready (direct {ip})")
            return s
        except OSError as e:
            log(f"direct {ip} failed: {e}")
    raise OSError("no upstream available")


def pipe(src, dst, tag, counter):
    try:
        while True:
            d = src.recv(16384)
            if not d:
                break
            counter[tag] += len(d)
            dst.sendall(d)
            log(f"{tag} +{len(d)}B (total {counter[tag]}B)")
    except OSError as e:
        log(f"{tag} error {e}")
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle(cli, addr, proxy):
    log(f"client connected {addr[0]}:{addr[1]}")
    try:
        up = open_upstream(proxy)
    except OSError as e:
        log(f"upstream failed: {e}")
        cli.close()
        return
    counter = {"C->U": 0, "U->C": 0}
    threading.Thread(target=pipe, args=(cli, up, "C->U", counter), daemon=True).start()
    pipe(up, cli, "U->C", counter)
    try:
        cli.close()
    except OSError:
        pass
    try:
        up.close()
    except OSError:
        pass
    log(f"connection closed {addr[0]}:{addr[1]}  C->U={counter['C->U']}B  U->C={counter['U->C']}B")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--proxy", default="127.0.0.1:7892")
    args = ap.parse_args()

    proxy = None
    if args.proxy and args.proxy.lower() != "none":
        host, _, port = args.proxy.rpartition(":")
        proxy = (host or "127.0.0.1", int(port))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.listen, args.port))
    srv.listen(32)
    log(f"listening {args.listen}:{args.port} -> {UPSTREAM_HOST}:{UPSTREAM_PORT}")
    log(f"upstream proxy: {proxy if proxy else 'direct'}")
    try:
        while True:
            cli, addr = srv.accept()
            threading.Thread(target=handle, args=(cli, addr, proxy), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
