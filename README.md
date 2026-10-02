# ESP32-S3 Snapshot Kit — Multi-Sensor Capture, Triple-Channel Upload, Dual OTA

A production-style ESP-IDF firmware kit for ESP32-S3 camera boards:
**one codebase builds six firmware variants** by switching camera sensor and
SD-card policy at compile time, then captures, uploads over HTTP / Telegram /
MQTT, and updates itself over the air with rollback protection.

> Written for real deployments: every network call has a timeout and bounded
> retries, no hard-coded secrets, and a documented provisioning path for
> devices that ship without Wi-Fi credentials.

---

## Highlights

| | |
|---|---|
| **6-in-1 build matrix** | Camera (OV2640 / OV3660 / OV5640) × SD policy (none / optional / required) — verified **6/6 configurations build clean**. |
| **Triple upload path** | HTTP multipart POST, Telegram `sendPhoto`, MQTT event publish. Selectable per device; MQTT is off by default. |
| **Dual OTA** | Manual push (`POST /api/ota`) **and** boot-time auto-check against a JSON manifest — with A/B slots, MD5 pre-check and rollback. |
| **Zero-touch provisioning** | Unprovisioned devices fall back to SoftAP (`ESP32S3-Setup`) with an embedded web console; no app, no serial cable needed. |
| **Status without a screen** | WS2812 + board LED encode boot / ready / capture / upload / fatal, with a buzzer alert on fatal errors. |
| **Runtime config, not reflashes** | Wi-Fi, upload targets, admin key and OTA URL live in LittleFS `config.json` and can be changed over the web UI. |

---

## Hardware

| Item | Detail |
|---|---|
| Board | Freenove ESP32-S3-WROOM (ESP32-S3-**N16R8**) |
| Memory | 16 MB flash + **8 MB octal (OPI) PSRAM @ 80 MHz** |
| Camera | 24-pin FPC — **OV2640 (default) / OV3660 / OV5640**, selected at compile time |
| Storage | On-board microSD via **1-bit SDMMC** — CLK 39 / CMD 38 / DAT0 40 |
| Indicators | WS2812 on GPIO48 (RMT driven), green LED on GPIO2 |
| Toolchain | **ESP-IDF v5.5.1**, target `esp32s3` |

Pin mapping is centralised in `main/pins_config.h` (single source of truth);
a full wiring table lives in `docs/wiring.md`.

### Build variants

```
CONFIG_CAM_OV2640 / OV3660 / OV5640   ×   SD_MODE_NONE / OPTIONAL / REQUIRED
```

| Variant | Firmware size |
|---|---|
| `ov2640_sd_none` | 1 238 016 B |
| `ov2640_sd_optional` | 1 302 448 B |
| `ov2640_sd_required` | 1 302 432 B |
| `ov3660_optional` | 1 304 944 B |
| `ov5640_optional` | 1 305 136 B |
| `full_features` (incl. MJPEG stream) | 1 333 632 B |

> Sensor selection is **compile-time** by design: the framebuffer geometry and
> the driver set are fixed at `esp_camera_init()`, so mixing sensors in one
> binary would waste PSRAM and invite silent misconfiguration.

---

## Architecture

```
                 ┌──────────────── ESP32-S3 ────────────────┐
  PIR / button ─▶│ trigger_fsm  (cooldown, debounce, single  │
  remote / timer │              serial trigger task)         │
                 │        │                                  │
                 │        ▼                                  │
                 │  snapshot_pipeline  grab → validate → fan-out
                 │        │                                  │
                 │        ▼                                  │
                 │  upload_queue   path mode (SD) or buffer   │
                 │        │        (PSRAM) with retry         │
                 │        ├─▶ upload_http      (multipart)    │
                 │        ├─▶ upload_telegram  (sendPhoto)    │
                 │        └─▶ mqtt_hal         (event JSON)   │
                 │                                           │
                 │  web_server + web/index.html  (config UI) │
                 │  ota_check (boot)  /  ota_hal (manual)    │
                 └───────────────────────────────────────────┘
```

Every module is a self-contained HAL-style unit (`camera_hal`, `sd_hal`,
`wifi_hal`, `led_hal`, `buzzer_hal`, `time_sync`, `config_store`, `logger`),
and `main.c` does **startup orchestration only** — no business logic.

---

## Quick start

```bash
# 1. Toolchain
. $IDF_PATH/export.sh              # ESP-IDF v5.5.1

# 2. Project
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor       # Ctrl+] to exit the monitor
```

**Build the whole matrix (all 6 variants) in one shot:**

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_matrix.ps1
```

Artifact name is `build/esp32s3_snapshot_kit.bin` (the project name deliberately
differs from the folder name).

### First-time provisioning

1. Unprovisioned device starts a SoftAP: **`ESP32S3-Setup`**, password `12345678`.
2. Join it and open **`http://192.168.4.1/`** (or `http://esp32s3cam.local/`).
3. Enter your Wi-Fi SSID/password (plus Telegram token / HTTP endpoint if used).
4. The device reboots into STA mode — ready when the log shows `armed=1` and the
   LED enters the slow-blink READY pattern.

---

## OTA — two independent paths

| Path | Trigger | Notes |
|---|---|---|
| **Manual push** | `POST /api/ota` | Immediate, idempotent; MD5 pre-check before writing. |
| **Boot auto-check** | Cold boot | Fetches a JSON manifest, compares versions, updates only if strictly newer. |

* Runs in its **own task with an 8 KB stack** (OTA needs mbedTLS/SHA-256; the
  original in-`app_main` implementation overflowed the 8 KB main stack).
* The version is read from the `esp_app_desc_t` inside the image, and the
  **single source of truth is `main/version.h`**.
* Manifest format: `{"version": "...", "url": "...", "md5": "..."}`
* A local mock server (`rest_mock_server/server.py`, port 8080) serves
  `/ota.json` + `/firmware/*.bin` so OTA can be tested without a real server.
* Post-update restart takes roughly **10–60 s** depending on the partition size.

---

## Flash layout

```
factory     app (current)
ota_0       app slot A
ota_1       app slot B
otadata     A/B selection      ← must exist, see below
littlefs    runtime config / logs
```

> ⚠️ **Hard-won gotcha:** if `otadata` is missing from the partition table (or is
> only one sector instead of `0x2000`), OTA reports success forever and the
> version never changes. Shifting offsets to make room also means shrinking
> `littlefs` and re-provisioning the device, because partition changes wipe
> NVS + LittleFS.

---

## Engineering notes

The bugs below are documented because they are the kind that only reproduce on
real hardware — and each one cost hours:

* **"Device keeps rebooting / reflashes in a loop."** The image header version
  had become a git-describe string (e.g. `676dc691-dirty`), which the version
  parser read as `676.0.0` → the device saw a "newer" firmware on every boot.
  Fix: keep CMake deriving the version from `version.h` and `idf.py fullclean`.
* **"OTA always succeeds but the version never changes."** Missing `otadata`
  partition (see the flash-layout warning above).
* **No hard-coded secrets.** `admin_key` and the Telegram token come from
  `/config.json`; logs print at most the first 6 characters.
* **No `while(!connected)` anywhere.** Every blocking operation is bounded:
  60 s STA timeout before falling back to SoftAP, bounded retries with backoff
  on every upload path.
* **Single framebuffer return point.** The frame pointer is nulled immediately
  after the queue copies it — a double-free was the source of random resets
  during burst captures.
* **Cooldown is enforced in one serial trigger task**, so two trigger sources
  can never pass the gate concurrently and produce overlapping captures.
* **No Arduino.** Native IDF APIs only.

Detailed test procedures and debugging history: `docs/testing_full.md`
(authoritative), `docs/debug_notes.md`, `docs/troubleshooting.md`.

---

## Repository layout

```
main/                 firmware sources (HAL modules + pipeline)
main/web/index.html   embedded single-page configuration console
docs/                 wiring, sensor notes, SD modes, API, test plans
partitions.csv        factory + 2×OTA + littlefs + otadata
rest_mock_server/     mock OTA server, firmware publisher
tools/                build_matrix.ps1, test_api.py, parse_log.py
sdkconfig.defaults    reference configuration
```

---

## License & contact

Provided as a working reference implementation for ESP32-S3 camera products.
Adapt `main/pins_config.h`, `partitions.csv` and `main/app_conf.h` to your own
hardware and delivery targets.

**Need ESP-IDF firmware, OTA architecture, or IoT upload pipelines built for
your product?**
→ Reach me on Upwork: `https://www.upwork.com/freelancers/~YOUR_PROFILE_ID`
