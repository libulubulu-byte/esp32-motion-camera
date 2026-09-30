#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
透明 TCP 中继：把局域网设备发往 api.telegram.org:443 的连接，经 PC 的代理/VPN 转出去。

--------------------------------------------------------------------------
为什么需要它（2026-09-27 实测）
--------------------------------------------------------------------------
本工程设备端 Telegram 报 `err=perform_0x7002`（ESP_ERR_HTTP_CONNECT），而排除法显示：
  · 设备局域网通道全绿（HTTP 上传 200 / MQTT connected / OTA 清单 200）
  · 设备公网 UDP 通（[SYS ] sntp ok）
  · 只有到 api.telegram.org 的 TCP 连不上

原因链：
  1) 设备（ESP32）**没有代理能力** —— IDF 5.5.1 的 esp_http_client 里没有任何 proxy 字段；
  2) 国产网络直连 api.telegram.org 会被阻断；
  3) 想借 PC 的 VPN：设备连 PC 的移动热点，ICS 的共享源指向 TUN（v2cloud）。
     实测即使共享源=TUN、`本地连接* 2`/`v2cloud` 的 IP 转发都 Enabled，
     **代理客户端仍然不处理"来自其它主机"的转发包** —— 设备依旧 0x7002。

本中继绕开这一限制：它是跑在 PC 上的**普通本机进程**，它自己发起的连接天然走 VPN；
设备只要连到它即可。**TLS 是设备↔Telegram 端到端**的（中继只搬字节，不碰证书），
所以这就等于真正验证了设备自身的 Telegram 实现，而不是绕过它。

--------------------------------------------------------------------------
配套的两条系统改动（都要管理员）
--------------------------------------------------------------------------
  1) 在 PC 的 hosts 里加一行（让 ICS 的 DNS 把域名交给本中继）：
         192.168.137.1  api.telegram.org
     ⚠️ 上游必须用"真实 IP 或走代理"，否则会自环（中继连回自己）。
        本脚本默认走 HTTP 代理的 CONNECT（由代理侧解析域名），天然避免自环；
        代理不可用时退回**直连真实 IP**（内置常见 Telegram API IP）。
  2) 放行热点子网到本机 443 的入站：
         New-NetFirewallRule -DisplayName "ESP32 telegram relay" -Direction Inbound `
           -Action Allow -Protocol TCP -LocalPort 443 -RemoteAddress 192.168.137.0/24 -Profile Any

用法：
    python telegram_relay.py                      # 监听 0.0.0.0:443，上游走 127.0.0.1:7892
    python telegram_relay.py --proxy none         # 不用代理，直连真实 IP
    python telegram_relay.py --port 8443          # 换端口（hosts 里要跟着写）
"""
import argparse
import socket
import threading
import time

UPSTREAM_HOST = "api.telegram.org"
UPSTREAM_PORT = 443

# 代理不可用时的直连兜底：Telegram Bot API 的常见 IP（写死避免 self-loop，见文件头）
FALLBACK_IPS = ["149.154.166.110", "149.154.167.220", "149.154.175.50"]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] [relay] {msg}", flush=True)


def upstream_ips():
    """拿到上游候选 IP：先真解析（排除本机热点网段，防自环），再补兜底列表。"""
    ips = []
    try:
        for ai in socket.getaddrinfo(UPSTREAM_HOST, UPSTREAM_PORT, socket.AF_INET):
            ip = ai[4][0]
            if ip.startswith("192.168.137.") or ip.startswith("127."):
                continue          # ★ hosts 改写后的结果，绝不能用作上游
            if ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    for ip in FALLBACK_IPS:
        if ip not in ips:
            ips.append(ip)
    return ips


def open_upstream(proxy):
    """建立到 Telegram 的连接。优先 HTTP 代理 CONNECT（代理侧解析域名，最稳）。"""
    if proxy:
        try:
            s = socket.create_connection(proxy, timeout=8)
            # ★ CONNECT 建好后必须**去掉超时**：TLS 会话与 getUpdates 长轮询
            #   经常静默 >8s，带着 create_connection 的 8s 超时会被中继误判为
            #   出错并掐断连接，设备侧表现为 open_0x7002 / status_-1。
            s.settimeout(None)
            req = (
                f"CONNECT {UPSTREAM_HOST}:{UPSTREAM_PORT} HTTP/1.1\r\n"
                f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}\r\n\r\n"
            ).encode()
            s.sendall(req)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = s.recv(256)
                if not d:
                    raise OSError("代理提前关闭连接")
                buf += d
            status = buf.split(b"\r\n", 1)[0].decode("latin1", "replace")
            if " 200" not in status:
                raise OSError(f"代理 CONNECT 失败: {status}")
            log(f"上游就绪（经代理 {proxy[0]}:{proxy[1]}）")
            return s
        except OSError as e:
            log(f"代理方式失败（{e}）-> 退回直连")

    for ip in upstream_ips():
        try:
            s = socket.create_connection((ip, UPSTREAM_PORT), timeout=8)
            s.settimeout(None)   # 同上：连上后必须阻塞等待，否则会掐断长连接
            log(f"上游就绪（直连 {ip}）")
            return s
        except OSError as e:
            log(f"直连 {ip} 失败: {e}")
    raise OSError("没有可用的上游")


def pipe(src, dst):
    try:
        while True:
            d = src.recv(16384)
            if not d:
                break
            dst.sendall(d)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle(cli, addr, proxy):
    log(f"客户端接入 {addr[0]}:{addr[1]}")
    try:
        up = open_upstream(proxy)
    except OSError as e:
        log(f"上游建立失败，断开本连接: {e}")
        cli.close()
        return
    threading.Thread(target=pipe, args=(cli, up), daemon=True).start()
    pipe(up, cli)
    try:
        cli.close()
    except OSError:
        pass
    try:
        up.close()
    except OSError:
        pass
    log(f"连接结束 {addr[0]}:{addr[1]}")


def main():
    ap = argparse.ArgumentParser(description="api.telegram.org 透明中继（给无代理能力的设备用）")
    ap.add_argument("--listen", default="0.0.0.0", help="监听地址（默认所有网卡，含热点 192.168.137.1）")
    ap.add_argument("--port", type=int, default=UPSTREAM_PORT, help="监听端口（默认 443）")
    ap.add_argument("--proxy", default="127.0.0.1:7892",
                    help='上游 HTTP 代理 host:port；填 none 表示不用代理')
    args = ap.parse_args()

    proxy = None
    if args.proxy and args.proxy.lower() != "none":
        host, _, port = args.proxy.rpartition(":")
        proxy = (host or "127.0.0.1", int(port))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.listen, args.port))
    srv.listen(32)

    log(f"监听 {args.listen}:{args.port} -> {UPSTREAM_HOST}:{UPSTREAM_PORT}")
    log(f"上游代理: {proxy if proxy else '不使用（直连）'}")
    log("等待设备连接…（Ctrl+C 退出）")

    try:
        while True:
            cli, addr = srv.accept()
            threading.Thread(target=handle, args=(cli, addr, proxy), daemon=True).start()
    except KeyboardInterrupt:
        log("退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
