#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：极简 SNTP 应答器（给热点上的 ESP32 对时用）。

背景：设备经 PC 移动热点的 ICS 出去对 pool.ntp.org / 公共 NTP 的 UDP 转发不可靠
（实测时通时不通），而 Telegram 的 TLS 证书校验**必须有正确时间**。
所以在 PC 上直接应答 SNTP —— 设备把 ntp_server 改成 192.168.137.1 即可。

监听 192.168.137.1:123（UDP）。收到 48 字节 NTP 请求后按 RFC4330 回一个
Mode=4(server) / Stratum=1 / 回填 originate 时间戳 的应答。

用法： python _ntp_server.py
"""
import socket
import struct
import time

BIND = ("192.168.137.1", 123)
NTP_EPOCH_DELTA = 2208988800  # 1900-01-01 -> 1970-01-01


def now_ntp():
    """返回 (秒, 小数) 的 NTP 时间戳。"""
    t = time.time() + NTP_EPOCH_DELTA
    sec = int(t)
    frac = int((t - sec) * (1 << 32))
    return sec & 0xFFFFFFFF, frac & 0xFFFFFFFF


def log(msg):
    print("[%s] [ntp] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(BIND)
    log("listening on %s:%d (udp)" % BIND)

    while True:
        try:
            data, addr = s.recvfrom(1024)
            if len(data) < 48:
                continue
            # 客户端请求的 transmit 时间戳（字节 40..47）
            orig = data[40:48]

            ref_sec, ref_frac = now_ntp()
            tx_sec, tx_frac = now_ntp()

            resp = bytearray(48)
            resp[0] = 0x24          # LI=0, VN=4, Mode=4(server)
            resp[1] = 1             # stratum=1
            resp[2] = 4             # poll
            resp[3] = 0xEC          # precision = -20
            struct.pack_into(">I", resp, 4, 0)          # root delay
            struct.pack_into(">I", resp, 8, 0x00010000)  # root dispersion
            resp[12:16] = b"LOCL"                        # reference id
            struct.pack_into(">II", resp, 16, ref_sec, ref_frac)   # reference ts
            resp[24:32] = orig                                     # ★ originate = 客户端发来的
            struct.pack_into(">II", resp, 32, tx_sec, tx_frac)     # receive ts
            struct.pack_into(">II", resp, 40, tx_sec, tx_frac)     # transmit ts

            s.sendto(bytes(resp), addr)
            log("reply -> %s:%d  %s" % (addr[0], addr[1],
                                        time.strftime("%Y-%m-%d %H:%M:%S")))
        except Exception as e:      # noqa: BLE001 - 常驻进程要能自愈
            log("error: %r" % (e,))


if __name__ == "__main__":
    main()
