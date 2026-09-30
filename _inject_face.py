#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：把一张 PNG/任意图片转成 VGA JPEG，然后注入设备的 /api/detect_raw。"""
import glob
import io
import os

from PIL import Image

import _devreq

SRC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_inject")
OUT_JPG = os.path.join(SRC_DIR, "face.jpg")


def pick_source():
    files = sorted(glob.glob(os.path.join(SRC_DIR, "*.png")) +
                   glob.glob(os.path.join(SRC_DIR, "*.jpg")))
    files = [f for f in files if os.path.basename(f) != "face.jpg"]
    if not files:
        raise SystemExit("no source image in " + SRC_DIR)
    return files[-1]


def to_jpeg(src):
    im = Image.open(src).convert("RGB")
    im.thumbnail((640, 480))
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=85)
    return im.size, buf.getvalue()


def main():
    src = pick_source()
    size, data = to_jpeg(src)
    with open(OUT_JPG, "wb") as f:
        f.write(data)
    print("source : %s" % os.path.basename(src))
    print("jpeg   : %dx%d  %d bytes (%.1f KB)" % (size[0], size[1], len(data), len(data) / 1024.0))
    print("-> POST /api/detect_raw")
    h, b = _devreq.request("POST", "/api/detect_raw", data, "image/jpeg", timeout=60)
    print("resp   :", h.splitlines()[0] if h else "(no response)")
    print("body   :", b.decode("utf-8", "replace")[:400])


if __name__ == "__main__":
    main()
