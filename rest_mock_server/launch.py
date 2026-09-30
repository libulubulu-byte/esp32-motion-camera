#!/usr/bin/env python3
"""后台启动 server.py，写日志与 PID 文件。

单独成文件是为了避开 start.bat 里内联 Python 的路径转义坑
（Windows 路径以反斜杠结尾时，r'...\\' 会吃掉收尾引号）。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "server.log")
PID = os.path.join(HERE, "server.pid")
SERVER = os.path.join(HERE, "server.py")

# DETACHED_PROCESS 让服务脱离父控制台独立存活，
# CREATE_NEW_PROCESS_GROUP 便于将来按组结束整棵进程树。
flags = 0x00000008 | 0x00000200

# ★ 必须显式把 PORT 传下去。server.py 是 `int(os.environ.get("PORT", 8080))`，
#   而 bat 里的 `set "PORT=8081"` 只存在于 bat 自己的环境里；本脚本是子进程，
#   但 os.environ **会继承**，所以这里显式取默认值再写回，语义最清楚：
#   bat 设了就用 bat 的，没设就退回 8081（与 app_conf.h 的 APP_OTA_CHECK_URL 一致）。
env = dict(os.environ)
env.setdefault("PORT", "8081")

with open(LOG, "ab", buffering=0) as f:
    proc = subprocess.Popen(
        [sys.executable, "-u", SERVER],
        stdout=f,
        stderr=f,
        cwd=HERE,
        env=env,
        creationflags=flags,
    )

with open(PID, "w") as f:
    f.write(str(proc.pid))

print(proc.pid)
