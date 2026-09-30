#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：后台持续监听串口并带时间戳写入 _serial.log。

要点：
  · 打开后立刻 DTR=False / RTS=False —— **不进复位、不进口模式**，
    设备保持当前运行状态（用 DTR/RTS 组合会触发 EN/GPIO0，把设备重启）。
  · 断线自动重开，方便长时间挂机。
用法： python _serialmon.py          （前台跑；被 Start-Process 拉起则后台跑）
"""
import os
import time

import serial

PORT = os.environ.get("SERIAL_PORT", "COM9")
BAUD = int(os.environ.get("SERIAL_BAUD", "115200"))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_serial.log")


def main():
    with open(OUT, "a", encoding="utf-8") as f:
        f.write("\n===== monitor start %s =====\n" % time.strftime("%H:%M:%S"))
        f.flush()
        while True:
            try:
                s = serial.Serial(PORT, BAUD, timeout=0.3,
                                  rtscts=False, dsrdtr=False)
                s.setDTR(False)
                s.setRTS(False)
                f.write("---- port opened %s ----\n" % time.strftime("%H:%M:%S"))
                f.flush()
                buf = b""
                while True:
                    d = s.read(4096)
                    if not d:
                        continue
                    buf += d
                    while b"\n" in buf:
                        line, _, buf = buf.partition(b"\n")
                        txt = line.decode("utf-8", "replace").rstrip("\r")
                        if txt.strip():
                            f.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), txt))
                            f.flush()
            except Exception as e:      # noqa: BLE001 - 长时间挂机要能自愈
                f.write("!! serial error: %r\n" % (e,))
                f.flush()
                time.sleep(2)


if __name__ == "__main__":
    main()
