#!/usr/bin/env python3
"""REST test backend - pairs with the REST backend of the esp32 project
AWS_mqtt_REST_API.

Device behaviour (see main/rest_api.c):
  POST <host>:<port><rest_path>      report JSON:
      {"device","version","temperature","humidity","rssi","uptime","lamp"}
  GET  <host>:<port><rest_cmd_path>  poll for a lamp command, expects:
      {"lamp":"ON"} / {"lamp":"OFF"} or 204 No Content

This service additionally serves a browser dashboard:
  GET  /               dashboard (live values + history chart + lamp switch)
  GET  /api/latest     newest report
  GET  /api/history    most recent N reports (200 by default)
  POST /api/lamp       set the lamp manually (effective on the next poll)
  GET  /api/auto       ON/OFF (hand lamp commands to auto mode)
"""
import hashlib
import json
import os
import socket
import sqlite3
import threading
import time
from datetime import datetime, timezone
from urllib.parse import urlparse

from flask import Flask, Response, g, jsonify, request, send_file

# The database defaults to the script directory, so it just runs on Windows
# with no volume to mount.
DB_PATH = os.environ.get(
    "DB_PATH", os.path.join(os.path.dirname(os.path.abspath(__file__)), "telemetry.db")
)
# Report endpoint; must match APP_REST_REPORT_PATH in the firmware
REPORT_PATH = os.environ.get("REPORT_PATH", "/report")

# Extra aliases for the same handler. Why: the upload URL lives in the device's
# NVS (/config.json -> http_url), and a device configured before this server
# existed may point at /upload. A path mismatch shows up as an innocuous-looking
# 404 in the firmware log:
#     [UPLOW] http fail attempt=0 err=http_status_404
# which then retries 3x with backoff and finally DROPS the photo. Serving both
# names costs nothing and removes a whole class of "why is every upload 404".
# Set REPORT_ALIASES="" to disable if you want a strict single path.
_default_aliases = "/upload"
REPORT_ALIASES = [
    p.strip()
    for p in os.environ.get("REPORT_ALIASES", _default_aliases).split(",")
    if p.strip()
]
# Lamp command poll endpoint; must match APP_REST_CMD_PATH in the firmware
CMD_PATH = os.environ.get("CMD_PATH", "/lampcmd")

app = Flask(__name__)
_lock = threading.Lock()

# Lamp command currently headed for the device. None means 204 (no command,
# the device leaves the lamp as it is).
_lamp_cmd = None


# ----------------------------------------------------------------------
# Database
# ----------------------------------------------------------------------
def db():
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH, timeout=10)
        g.db.row_factory = sqlite3.Row
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS reports (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          REAL    NOT NULL,
            device      TEXT,
            version     TEXT,
            temperature REAL,
            humidity    REAL,
            rssi        INTEGER,
            uptime      INTEGER,
            lamp        TEXT
        )
        """
    )
    conn.execute("CREATE INDEX IF NOT EXISTS idx_reports_ts ON reports(ts)")
    conn.commit()
    conn.close()


def num(v, cast=float):
    try:
        return cast(v)
    except (TypeError, ValueError):
        return None


# ----------------------------------------------------------------------
# Device-facing endpoints
# ----------------------------------------------------------------------
@app.route(REPORT_PATH, methods=["POST"])
@app.route(REPORT_ALIASES[0] if REPORT_ALIASES else REPORT_PATH, methods=["POST"])
def report():
    """Receive a device report. The firmware only looks at the 2xx status.

    Served under REPORT_PATH and its aliases (see REPORT_ALIASES) so a device
    whose NVS still holds the other spelling does not 404 and drop photos.
    """
    data = request.get_json(silent=True)
    if data is None:
        data = {}
    ts = time.time()
    row = (
        ts,
        str(data.get("device", "")),
        str(data.get("version", "")),
        num(data.get("temperature")),
        num(data.get("humidity")),
        num(data.get("rssi"), int),
        num(data.get("uptime"), int),
        str(data.get("lamp", "")),
    )
    conn = db()
    conn.execute(
        "INSERT INTO reports (ts,device,version,temperature,humidity,rssi,uptime,lamp)"
        " VALUES (?,?,?,?,?,?,?,?)",
        row,
    )
    conn.commit()
    stamp = datetime.fromtimestamp(ts).strftime("%H:%M:%S")
    print(
        f"[{stamp}] REPORT {row[1]} v{row[2]} "
        f"T={row[3]} H={row[4]} rssi={row[5]} lamp={row[7]}",
        flush=True,
    )
    return jsonify({"ok": True}), 200


@app.route(CMD_PATH, methods=["GET"])
def lamp_cmd():
    """Device polls for a lamp command. Returns 204 when there is none."""
    global _lamp_cmd
    with _lock:
        cmd = _lamp_cmd
        if cmd is not None:
            _lamp_cmd = None  # deliver once and clear, so the device does not
                              # keep receiving the same command every poll
    if cmd is None:
        return Response(status=204)
    print(f"[{datetime.now():%H:%M:%S}] CMD   -> lamp={cmd}", flush=True)
    return jsonify({"lamp": cmd}), 200


# ----------------------------------------------------------------------
# Dashboard API
# ----------------------------------------------------------------------
@app.route("/api/latest")
def api_latest():
    row = db().execute("SELECT * FROM reports ORDER BY ts DESC LIMIT 1").fetchone()
    return jsonify(dict(row) if row else {})


@app.route("/api/history")
def api_history():
    limit = min(int(request.args.get("limit", 200)), 5000)
    rows = db().execute(
        "SELECT * FROM reports ORDER BY ts DESC LIMIT ?", (limit,)
    ).fetchall()
    return jsonify([dict(r) for r in reversed(rows)])


@app.route("/api/lamp", methods=["POST"])
def api_set_lamp():
    global _lamp_cmd
    state = str((request.get_json(silent=True) or {}).get("state", "")).upper()
    if state not in ("ON", "OFF", "NONE"):
        return jsonify({"error": "state must be ON / OFF / NONE"}), 400
    with _lock:
        _lamp_cmd = None if state == "NONE" else state
    return jsonify({"queued": state})


@app.route("/api/stats")
def api_stats():
    conn = db()
    n, = conn.execute("SELECT COUNT(*) FROM reports").fetchone()
    first, = conn.execute("SELECT MIN(ts) FROM reports").fetchone()
    last, = conn.execute("SELECT MAX(ts) FROM reports").fetchone()
    return jsonify(
        {
            "count": n,
            "since": first,
            "last": last,
            "age_s": (time.time() - last) if last else None,
        }
    )


@app.route("/api/clear", methods=["POST"])
def api_clear():
    conn = db()
    conn.execute("DELETE FROM reports")
    conn.commit()
    return jsonify({"ok": True})


DASHBOARD = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 REST Server</title>
<style>
  :root{--bg:#0f1115;--card:#181b22;--line:#272b35;--fg:#e6e8ee;--mut:#8b93a7;--acc:#4f8cff;--ok:#39d98a;--warn:#ffb648}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif}
  header{padding:16px 22px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:14px;flex-wrap:wrap}
  header h1{font-size:16px;margin:0;font-weight:600}
  .dot{width:9px;height:9px;border-radius:50%;background:#555;display:inline-block}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .dot.off{background:#e5484d}
  .mut{color:var(--mut);font-size:12px}
  main{padding:22px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));max-width:1400px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
  .card h2{margin:0 0 12px;font-size:13px;color:var(--mut);font-weight:600;letter-spacing:.4px;text-transform:uppercase}
  .big{font-size:30px;font-weight:600;font-variant-numeric:tabular-nums}
  .big small{font-size:14px;color:var(--mut);font-weight:400;margin-left:4px}
  .kv{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px dashed var(--line)}
  .kv:last-child{border-bottom:none}
  .kv span:first-child{color:var(--mut)}
  .row{display:flex;gap:10px;flex-wrap:wrap}
  button{flex:1;min-width:90px;padding:11px;border-radius:8px;border:1px solid var(--line);
         background:#20242e;color:var(--fg);font-size:14px;cursor:pointer;transition:.15s}
  button:hover{background:#2a2f3b;border-color:#3a4150}
  button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
  button.pri:hover{background:#3d7bf0}
  button.on{background:var(--ok);border-color:var(--ok);color:#04220f;font-weight:600}
  button.dan{color:#ff9a9a}
  #chart{width:100%;height:180px;display:block}
  .legend{display:flex;gap:16px;font-size:12px;color:var(--mut);margin-top:6px}
  .sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:middle}
  table{width:100%;border-collapse:collapse;font-size:12px;font-variant-numeric:tabular-nums}
  th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
  th{color:var(--mut);font-weight:500;position:sticky;top:0;background:var(--card)}
  .scroll{max-height:320px;overflow:auto}
  .grid-full{grid-column:1/-1}
</style>
</head>
<body>
<header>
  <h1>ESP32 REST Server</h1>
  <span class="dot" id="dot"></span>
  <span class="mut" id="link">Waiting for device...</span>
  <span class="mut" id="stats" style="margin-left:auto"></span>
</header>

<main>
  <div class="card">
    <h2>Temperature</h2>
    <div class="big" id="temp">--<small>°C</small></div>
  </div>
  <div class="card">
    <h2>Humidity</h2>
    <div class="big" id="hum">--<small>%RH</small></div>
  </div>
  <div class="card">
    <h2>Device</h2>
    <div class="kv"><span>Device ID</span><b id="dev">--</b></div>
    <div class="kv"><span>Firmware</span><b id="ver">--</b></div>
    <div class="kv"><span>RSSI</span><b id="rssi">--</b></div>
    <div class="kv"><span>Uptime</span><b id="up">--</b></div>
    <div class="kv"><span>Reported lamp</span><b id="replamp">--</b></div>
  </div>
  <div class="card">
    <h2>Lamp Control</h2>
    <div class="row">
      <button id="bON" class="pri" onclick="setLamp('ON')">Turn On</button>
      <button id="bOFF" class="pri" onclick="setLamp('OFF')">Turn Off</button>
      <button onclick="setLamp('NONE')">Cancel</button>
    </div>
    <p class="mut" style="margin:12px 0 0">
      Commands are queued once and cleared as soon as the device picks them up
      on its next poll (1s by default).
    </p>
    <div class="kv" style="margin-top:10px"><span>Queued command</span><b id="queued">none</b></div>
  </div>

  <div class="card grid-full">
    <h2>History (last 120 points)</h2>
    <canvas id="chart"></canvas>
    <div class="legend">
      <span><i class="sw" style="background:#ff7043"></i>Temperature °C</span>
      <span><i class="sw" style="background:#4f8cff"></i>Humidity %RH</span>
    </div>
  </div>

  <div class="card grid-full">
    <h2>Recent Reports</h2>
    <div class="scroll">
      <table>
        <thead><tr><th>Time</th><th>Temp</th><th>Humidity</th><th>RSSI</th><th>Lamp</th><th>Version</th></tr></thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>
    <div class="row" style="margin-top:12px">
      <button class="dan" onclick="clearAll()">Clear history</button>
    </div>
  </div>
</main>

<script>
const $ = id => document.getElementById(id);
let queued = null;

function fmtUptime(s){
  if(s==null) return '--';
  const d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+(h?h+'h ':'')+m+'m';
}

function setLamp(state){
  fetch('/api/lamp',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({state})}).then(r=>r.json()).then(j=>{
      queued = state==='NONE' ? null : state;
      $('queued').textContent = queued ?? 'none';
      toast(state==='NONE'?'Queued command cancelled':'Queued '+state+', waiting for device');
    });
}

function toast(msg){
  const el=$('stats');
  const bak=el.dataset.msg||'';
  el.dataset.msg=msg;
  el.textContent=msg;
  clearTimeout(window._t);
  window._t=setTimeout(()=>{el.textContent=bak;},2500);
}

function clearAll(){
  if(!confirm('Clear all history records?')) return;
  fetch('/api/clear',{method:'POST'}).then(()=>refresh(true));
}

const hist=[];

function refresh(full){
  fetch('/api/history?limit=200').then(r=>r.json()).then(rows=>{
    if(rows.length){
      const d=rows[rows.length-1];
      const fresh = !window._lastTs || d.ts!==window._lastTs;
      window._lastTs = d.ts;

      $('temp').innerHTML=(d.temperature??'--')+'<small>°C</small>';
      $('hum').innerHTML=(d.humidity??'--')+'<small>%RH</small>';
      $('dev').textContent=d.device||'--';
      $('ver').textContent=d.version||'--';
      $('rssi').textContent=d.rssi!=null?d.rssi+' dBm':'--';
      $('up').textContent=fmtUptime(d.uptime);
      $('replamp').textContent=d.lamp||'--';

      const age=Date.now()/1000-d.ts;
      const online=age<10;
      $('dot').className='dot '+(online?'on':'off');
      $('link').textContent=online
        ? 'Online · last report '+age.toFixed(0)+'s ago'
        : 'Offline · last report '+fmtUptime(age)+' ago';

      if(fresh){
        // The reported lamp state matches the queued command -> it was consumed
        if(queued && (d.lamp===queued)){
          queued=null; $('queued').textContent='none';
        }
      }
    }
    if(full || hist.length===0 || rows.length){
      hist.length=0; rows.forEach(r=>hist.push(r));
      $('tbody').innerHTML = rows.slice(-60).reverse().map(r=>{
        const t=new Date(r.ts*1000).toLocaleTimeString('en-GB',{hour12:false});
        return `<tr><td>${t}</td><td>${r.temperature??'--'}</td>
                <td>${r.humidity??'--'}</td><td>${r.rssi??'--'}</td>
                <td>${r.lamp||'--'}</td><td>${r.version||'--'}</td></tr>`;
      }).join('');
      draw();
    }
  });

  fetch('/api/stats').then(r=>r.json()).then(s=>{
    if(!$('stats').dataset.msg){
      $('stats').textContent = s.count?`${s.count} records`:'';
    }
  });
}

function draw(){
  const c=$('chart'), ctx=c.getContext('2d');
  const dpr=window.devicePixelRatio||1;
  const w=c.clientWidth, h=180;
  c.width=w*dpr; c.height=h*dpr; ctx.scale(dpr,dpr);

  const data=hist.slice(-120);
  ctx.clearRect(0,0,w,h);
  if(data.length<2) return;

  const T=data.map(d=>d.temperature).filter(v=>v!=null);
  const H=data.map(d=>d.humidity).filter(v=>v!=null);
  const all=T.concat(H);
  let lo=Math.min(...all), hi=Math.max(...all);
  const pad=Math.max(1,(hi-lo)*0.15); lo-=pad; hi+=pad;

  const X=i=>10+(w-20)*i/(data.length-1);
  const Y=v=>h-16-(h-32)*(v-lo)/(hi-lo||1);

  // Grid
  ctx.strokeStyle='#272b35'; ctx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const y=12+(h-28)*i/4;
    ctx.beginPath(); ctx.moveTo(10,y); ctx.lineTo(w-10,y); ctx.stroke();
    ctx.fillStyle='#8b93a7'; ctx.font='10px sans-serif';
    ctx.fillText((hi-(hi-lo)*i/4).toFixed(1), 12, y-3);
  }

  const line=(key,color)=>{
    ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath();
    let started=false;
    data.forEach((d,i)=>{
      const v=d[key];
      if(v==null){started=false;return;}
      const x=X(i), y=Y(v);
      started?ctx.lineTo(x,y):ctx.moveTo(x,y);
      started=true;
    });
    ctx.stroke();
  };
  line('temperature','#ff7043');
  line('humidity','#4f8cff');
}

window.addEventListener('resize',draw);
refresh(true);
setInterval(refresh,2000);
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return Response(DASHBOARD, mimetype="text/html")


# ----------------------------------------------------------------------
# OTA firmware hosting
#
# Serves .bin images so an ESP32 can pull them over plain HTTP:
#   GET /firmware/manifest.json   -> {"file","md5","size"} of the newest .bin
#   GET /firmware/<name>.bin      -> the image itself
#
# The device flow is:
#   1. build the firmware, drop the .bin into ./firmware/
#   2. POST /api/ota on the device with {"url":"http://<pc-ip>:<port>/firmware/<name>.bin"}
#      (optionally adding "md5" taken from manifest.json)
#
# Keep this simple on purpose: it is a bench tool, not a production CDN.
# ----------------------------------------------------------------------
FIRMWARE_DIR = os.environ.get(
    "FIRMWARE_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware"),
)


def firmware_dir():
    """Create the firmware directory lazily so a fresh checkout just works."""
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    return FIRMWARE_DIR


def list_firmware():
    """All .bin files, newest first (by mtime)."""
    try:
        names = [n for n in os.listdir(FIRMWARE_DIR) if n.lower().endswith(".bin")]
    except OSError:
        return []
    entries = []
    for n in names:
        p = os.path.join(FIRMWARE_DIR, n)
        try:
            st = os.stat(p)
        except OSError:
            continue
        entries.append({"name": n, "size": st.st_size, "mtime": st.st_mtime})
    entries.sort(key=lambda e: e["mtime"], reverse=True)
    return entries


# ----------------------------------------------------------------------
# Reading the version out of an ESP32 app image
#
# Why parse instead of hand-writing ota.json: a hand-kept version is a trap.
# If you publish a .bin built from 0.1.2 but forget to bump the JSON (or bump
# it too high), the device re-downloads the *same* image on every boot - flash,
# reboot, flash, reboot. That is slow to notice and easy to misdiagnose.
#
# The app image carries its own version, so read it. Layout (stable across
# IDF 4.x/5.x, defined in esp_app_format.h):
#
#   0x00  uint8   magic            0xE9
#   0x01  uint8   segment_count
#   0x02  uint8   spi_mode
#   0x03  uint8   spi_speed_size
#   0x04  uint32  entry_addr
#   0x08  uint32  wp_pin
#   0x0C  uint8[3] spi_pin_drv
#   0x0F  uint16  chip_id
#   0x11  uint8   min_chip_rev
#   0x12  uint16  min_chip_rev_full
#   0x14  uint16  max_chip_rev_full
#   0x16  uint8[4] reserved
#   0x1A  uint8   hash_appended
#   0x1B  uint8[5] reserved
#   0x20  esp_app_desc_t:
#           0x20  uint32 magic_word       0xABCD5432
#           0x24  uint32 secure_version
#           0x28  uint8[4] reserv1        (0x28..0x2B)
#           0x2C  uint32 reserv2          (0x2C..0x2F)   <-- easy to miss!
#           0x30  char[32] version        <-- what we want
#           0x50  char[32] project_name
#           ...
#
# NOTE: version sits at absolute 0x30, NOT 0x2C. The struct has *two*
# reserved fields before it (reserv1[4] and a second 4-byte reserved word),
# so it is 0x10 into esp_app_desc_t. Verified against a real .bin: reading
# 0x2C yields NULs, which silently parses to "no version" and disables OTA.
# ----------------------------------------------------------------------
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFF = 0x30   # absolute: 0x20 + 0x10
APP_DESC_MAGIC_OFF = APP_DESC_OFFSET


_LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "::1", "0.0.0.0")

# 最近一次"非回环"请求用过的主机名。必须改写回环 url 时优先复用它 ——
# 那是设备/浏览器**刚刚真实访问通**的地址，比任何猜测都可靠。
_last_lan_host = None


def _note_request_host(host):
    """记住非回环请求用过的主机名（供 ota_json 改写回环 url 时复用）。

    单独写成一个函数只是为了把 `global` 放在一个干净的 scope 里 ——
    在 ota_json 内部既读又写同一个全局名会触发
    "SyntaxError: name is used prior to global declaration"。
    """
    global _last_lan_host
    _last_lan_host = host


def _lan_ip():
    """Best-effort LAN address of this machine that **other hosts can reach**.

    Deliberately NOT the usual "UDP-connect to 8.8.8.8 and read getsockname()"
    one-liner on its own: that returns the *default route's* source address, and
    with a VPN / TUN proxy installed (v2cloud, Clash and friends squat on
    198.18.0.0/15; WireGuard often 10.x) that is the virtual adapter's address.

    That failure is nasty because it is silent and looks exactly like "the server
    is not running": /ota.json is served fine and the version compares fine, then
    the device times out downloading the .bin from an address it can never reach.
    Observed for real on 2026-09-27: url came out as http://198.18.0.1:8081/...
    while the device sat on the 192.168.137.0/24 side of this PC.

    So: gather candidates from two cheap sources, drop addresses that can never
    be a LAN address, and prefer the ranges a device is actually likely to share.
    """
    cands = []
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # 不发包、不做 DNS，只让内核选一次路由
        cands.append(s.getsockname()[0])
    except OSError:
        pass
    finally:
        s.close()
    try:
        cands += socket.gethostbyname_ex(socket.gethostname())[2]
    except OSError:
        pass

    def usable(a):
        try:
            o = [int(x) for x in a.split(".")]
        except ValueError:
            return False
        if len(o) != 4 or o[0] in (0, 127) or o[3] in (0, 255):
            return False
        if o[0] == 169 and o[1] == 254:        # 169.254/16 链路本地（没拿到 DHCP）
            return False
        if o[0] == 198 and o[1] in (18, 19):   # 198.18.0.0/15 = TUN 代理的基准测试段
            return False
        return True

    def rank(a):
        o = [int(x) for x in a.split(".")]
        if o[0] == 192 and o[1] == 168:
            return 0    # 家用路由 / ICS / 手机热点（本工程现场就是这个）
        if o[0] == 10:
            return 1
        if o[0] == 172 and 16 <= o[1] <= 31:
            return 2
        return 3

    for a in sorted(dict.fromkeys(c for c in cands if usable(c)), key=rank):
        return a
    return None


def read_image_version(path):
    """Version string baked into a .bin, or None if it is not an app image.

    Returning None matters: the caller must NOT invent a version for an
    unparseable file, because a wrong version silently disables (or loops) OTA.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(APP_DESC_VERSION_OFF + 32)
    except OSError:
        return None
    if len(head) < APP_DESC_VERSION_OFF + 32:
        return None
    if head[0] != 0xE9:
        return None
    magic = int.from_bytes(head[APP_DESC_OFFSET:APP_DESC_OFFSET + 4], "little")
    if magic != APP_DESC_MAGIC:
        return None
    raw = head[APP_DESC_VERSION_OFF:APP_DESC_VERSION_OFF + 32]
    ver = raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()
    return ver or None


@app.route("/firmware/manifest.json")
def firmware_manifest():
    """Newest .bin plus its MD5, so the device can verify before writing flash.

    Passing md5 to POST /api/ota is optional in the firmware, but doing so makes
    a corrupted transfer fail *before* the OTA partition is touched.
    """
    entries = list_firmware()
    if not entries:
        return jsonify(
            {
                "ok": False,
                "error": "no firmware available",
                "hint": f"put a .bin into {FIRMWARE_DIR}",
            }
        ), 404

    newest = entries[0]
    path = os.path.join(FIRMWARE_DIR, newest["name"])
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)

    return jsonify(
        {
            "ok": True,
            "file": newest["name"],
            "md5": h.hexdigest(),
            "size": newest["size"],
            "version": read_image_version(path),
            "url_path": f"/firmware/{newest['name']}",
            "all": [{"name": e["name"], "size": e["size"]} for e in entries],
        }
    )


@app.route("/ota.json")
def ota_json():
    """Version manifest for the firmware's boot-time auto-update check.

    The device (main/ota_check.c, APP_OTA_CHECK_URL) does:

        GET /ota.json
        -> compare "version" with its own SNAP_FW_VERSION
        -> strictly newer ? download "url" and reboot : do nothing

    Shape (extra keys are ignored by the device):

        {"version": "0.1.2", "url": "http://host:port/firmware/x.bin", "md5": "..."}

    Why the URL is built from request.host instead of a hard-coded address:
    the same file then works whether the server runs on 8080 or 8081, on
    localhost or on 192.168.0.101, with no edit. A hard-coded host would
    silently point the device at the wrong machine the moment either the
    port or the LAN address changes.

    `version` comes from the .bin's own header (see read_image_version), so it
    always matches the image that `url` actually serves - no way to publish a
    mismatched pair.
    """
    entries = list_firmware()
    if not entries:
        return jsonify(
            {
                "ok": False,
                "error": "no firmware available",
                "hint": f"put a .bin into {FIRMWARE_DIR}",
            }
        ), 404

    newest = entries[0]
    path = os.path.join(FIRMWARE_DIR, newest["name"])

    version = read_image_version(path)
    if version is None:
        # Refuse rather than serve a version-less manifest: the device skips
        # entries it cannot compare, so a made-up value would be silently
        # ignored at best - or cause an update loop at worst.
        return jsonify(
            {
                "ok": False,
                "error": f"{newest['name']} has no readable app image header",
                "hint": "is this really an app .bin (not bootloader/partition-table)?",
            }
        ), 500

    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)

    base = request.host_url.rstrip("/")
    url = f"{base}/firmware/{newest['name']}"

    # ★ 回环地址必须换成真实局域网 IP，否则设备会去连它自己的 127.0.0.1。
    #
    # 这是个很容易踩的坑，因为它**只在有人用 localhost 打开过清单之后**才发作：
    # request.host_url 是"照抄请求方用的地址"，所以
    #   · 设备自己 GET http://192.168.0.101:8081/ota.json -> url 正确
    #   · 但浏览器 / curl / run_all.bat 的自检用 localhost 打一次 -> url 变成
    #     http://127.0.0.1:8081/firmware/x.bin，且会一直用到下次有人用 IP 请求为止
    # 症状：清单版本号明明更高、日志也显示 "newer x -> y -> 下载"，但下载阶段
    # 卡住直到超时（设备在连自己的 127.0.0.1，那里什么都没监听）。
    #
    # 注意 browser 用 127.0.0.1 还是 localhost 不重要，两种都算回环，都要换。
    host = urlparse(base).hostname or ""
    if host in _LOOPBACK_HOSTS:
        # 优先复用"最近一次非回环请求用过的主机"：那是设备/浏览器刚刚真实访问通的
        # 地址，比 _lan_ip() 的猜测可靠（多网卡 + VPN 时猜测会选错，见 _lan_ip 注释）。
        lan = _last_lan_host or _lan_ip()
        if lan:
            # 端口保持请求里的（可能被 PORT 环境变量改成别的），只换主机名
            port = urlparse(base).port
            base = f"http://{lan}:{port}" if port else f"http://{lan}"
            url = f"{base}/firmware/{newest['name']}"
            print(
                f"[{datetime.now():%H:%M:%S}] OTA   manifest 请求来自 {host}，"
                f"改写 url 主机为 {lan}（否则设备会连自己的 127.0.0.1）",
                flush=True,
            )
    else:
        # 记住设备真实用过的地址，供上面的回环改写复用。
        _note_request_host(host)

    print(
        f"[{datetime.now():%H:%M:%S}] OTA   ota.json -> v{version} {newest['name']}",
        flush=True,
    )
    return jsonify(
        {
            "version": version,
            "url": url,
            "md5": h.hexdigest(),
            "size": newest["size"],
        }
    )


@app.route("/firmware/<path:name>")
def firmware_download(name):
    """Stream a firmware image. Path traversal is rejected explicitly."""
    if not name.lower().endswith(".bin"):
        return jsonify({"ok": False, "error": "only .bin is served"}), 404

    full = os.path.abspath(os.path.join(FIRMWARE_DIR, name))
    root = os.path.abspath(FIRMWARE_DIR)
    # Guard against "../" escaping the firmware dir: the resolved path must
    # still live under the firmware root.
    if os.path.commonpath([full, root]) != root:
        return jsonify({"ok": False, "error": "invalid path"}), 400
    if not os.path.isfile(full):
        return jsonify({"ok": False, "error": f"{name} not found"}), 404

    size = os.path.getsize(full)
    print(f"[{datetime.now():%H:%M:%S}] OTA   serving {name} ({size} bytes)", flush=True)

    # send_file handles Range requests and correct Content-Length, which
    # esp_http_client relies on.
    return send_file(full, mimetype="application/octet-stream", as_attachment=False)


@app.route("/firmware/")
def firmware_index():
    """Human-readable listing - handy when checking from a phone browser."""
    entries = list_firmware()
    if not entries:
        return Response(
            f"<pre>no firmware in {FIRMWARE_DIR}\n\n"
            f"build it, then copy the .bin here:\n"
            f"  idf.py build\n"
            f"  copy build\\esp32S3_CAM.bin firmware\\</pre>",
            mimetype="text/html",
        )
    rows = "\n".join(f"{e['name']:<40} {e['size']:>10} bytes" for e in entries)
    return Response(
        f"<pre>firmware in {FIRMWARE_DIR}\n\n{rows}\n\n"
        f"manifest: /firmware/manifest.json</pre>",
        mimetype="text/html",
    )


if __name__ == "__main__":
    init_db()
    firmware_dir()
    port = int(os.environ.get("PORT", 8080))
    print(f"REST mock server listening on :{port}", flush=True)
    print(f"  device report:  POST http://<host-ip>:{port}{REPORT_PATH}", flush=True)
    print(f"  lamp poll:      GET  http://<host-ip>:{port}{CMD_PATH}", flush=True)
    print(f"  dashboard:      http://localhost:{port}/", flush=True)
    print(f"  ota firmware:   http://<host-ip>:{port}/firmware/  ({FIRMWARE_DIR})",
          flush=True)
    app.run(host="0.0.0.0", port=port, threaded=True)
