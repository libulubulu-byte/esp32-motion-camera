#!/usr/bin/env python3
r"""Copy a built firmware image into ./firmware/ and print the OTA command.

Why this exists: the OTA handler on the device wants a URL, and if you want
MD5 verification you also need the hash. Doing that by hand every time invites
typos (a wrong MD5 is *worse* than none - it rejects a perfectly good image).

Usage (from the rest_mock_server directory):
    python publish_firmware.py
    python publish_firmware.py ..\build\esp32S3_CAM.bin
    python publish_firmware.py --name v1.2.3.bin

The default source is ..\build\esp32s3_snapshot_kit.bin, or SNAP_BUILD_BIN if set.
"""
import argparse
import glob
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.environ.get("FIRMWARE_DIR", os.path.join(HERE, "firmware"))
BUILD_DIR = os.path.join(HERE, "..", "build")
# The app image name follows the project name (see CMakeLists.txt "project(...)").
# Look for the exact name first, then fall back to any single .bin so a rename
# does not silently break publishing.
DEFAULT_SRC = os.environ.get(
    "SNAP_BUILD_BIN", os.path.join(BUILD_DIR, "esp32s3_snapshot_kit.bin")
)


def resolve_src(explicit):
    """Pick the firmware to publish, with a helpful error when ambiguous."""
    if explicit:
        return os.path.abspath(explicit)
    if os.path.isfile(DEFAULT_SRC):
        return os.path.abspath(DEFAULT_SRC)
    found = sorted(glob.glob(os.path.join(BUILD_DIR, "*.bin")))
    if len(found) == 1:
        return os.path.abspath(found[0])
    return None


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description="publish a firmware .bin for OTA")
    ap.add_argument("src", nargs="?", default=DEFAULT_SRC, help="path to the .bin")
    ap.add_argument("--name", help="target file name (default: keep src name)")
    args = ap.parse_args()

    src = resolve_src(args.src if args.src != DEFAULT_SRC else None)
    if src is None:
        print(f"error: no firmware found in {os.path.abspath(BUILD_DIR)}", file=sys.stderr)
        print("build first:  idf.py build", file=sys.stderr)
        return 1
    if not os.path.isfile(src):
        print(f"error: {src} not found", file=sys.stderr)
        print("build first:  idf.py build", file=sys.stderr)
        return 1

    with open(src, "rb") as f:
        magic = f.read(1)
    if magic != b"\xe9":
        # The device checks this too (magic 0xE9) and would fail with
        # "not an esp image" - far better to catch it here.
        print(f"error: {src} is not an ESP image (magic {magic!r} != 0xE9)",
              file=sys.stderr)
        return 1

    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    name = args.name or os.path.basename(src)
    dst = os.path.join(FIRMWARE_DIR, name)
    shutil.copy2(src, dst)

    digest = md5_of(dst)
    size = os.path.getsize(dst)
    print(f"published {name}  ({size} bytes, {size / 1048576:.2f} MB)")
    print(f"md5       {digest}")

    # The version the device will read back out of this image. Worth printing
    # because it is the *only* thing the boot-time check compares: if it is not
    # what you expect, publishing is pointless (the device will either ignore
    # the image or re-download it forever).
    try:
        sys.path.insert(0, HERE)
        from server import read_image_version
        ver = read_image_version(dst)
    except Exception:
        ver = None
    print(f"version   {ver if ver else '(unreadable image header!)'}")
    print()

    # 默认 8081：与 start.bat / run_all.bat 以及 main/app_conf.h 的
    # APP_OTA_CHECK_URL 保持一致（8080 留给 tools/recv_server.py 收照片）。
    port = os.environ.get("PORT", "8081")
    print("Two ways to push this to a device:")
    print()
    print("1) manual, one device at a time:")
    print(f"     curl -X POST \"http://<pc-ip>/api/ota?url="
          f"http://<pc-ip>:{port}/firmware/{name}&md5={digest}\" \\")
    print("          -H \"X-Admin-Key: <your admin key>\"")
    print()
    print("2) automatic, on every boot (no action needed):")
    print(f"     the device GETs  http://<pc-ip>:{port}/ota.json")
    print("     and updates itself only if that version is strictly newer")
    print("     than its own. Requires two things:")
    print(f"       - APP_OTA_CHECK_URL in main/app_conf.h pointing at")
    print(f"         http://<pc-ip>:{port}/ota.json")
    print(f"       - the server running on port {port} (it serves both")
    print("         /ota.json and /firmware/*, no separate static server)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
