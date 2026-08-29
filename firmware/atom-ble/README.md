# AtomS3 Lite CSI + BLE firmware (`atom-ble`)

House CSI nodes (M5Stack AtomS3 Lite, ESP32-S3, 8MB) that keep live CSI + HTTP OTA and add continuous low-duty BLE iBeacon scanning for Blue Charm dog-collar tags.

## Why this exists

Live nodes already run a slim `atom-led` image of RuView `esp32-csi-node` **v0.8.4** (app **910736** bytes). Official RuView 0.8.4 is **~1.13 MB** and is **rejected** by `POST /ota` (`max_size` **921600**). This tree is a local copy of 0.8.4 with WASM/Tier 3 / display / mmWave stripped so NimBLE can fit.

**OTA app `.bin` must be ≤ 900000 bytes.** Live OTA will also refuse anything above 921600.

## Hardware / live constraints

| Item | Value |
|------|--------|
| Board | AtomS3 Lite, ESP32-S3 QFN56, 8MB flash |
| LED | GPIO **35** WS2812 — faint green online, flash red offline |
| CSI | Promiscuous, `WIFI_PS_NONE`, UDP to NVS `target_ip:target_port` (192.168.0.39:5005) |
| OTA | `GET :8032/ota/status`, `POST :8032/ota` with `Authorization: Bearer <ota_psk>` |
| CSI mute | PSK-gated `GET/POST :8032/csi/stop` and `/csi/start`. `POST /ota` also pauses CSI+BLE immediately after auth |
| OTA fail-closed | No PSK in NVS `security/ota_psk` → POST `/ota` and CSI-control routes rejected (RuView#596) |
| Partitions | Same 8MB dual-OTA table as live (`partitions.csv`, app at `0x20000`) |
| NVS | Reads existing `csi_cfg` + `security`. Writes only `csi_cfg/ble_scan` (u8). Does not wipe keys. |

Tonight’s OTA targets: node 1 upstairs-living `192.168.0.47`, node 2 stairs `192.168.0.49`.

## BLE (Blue Charm BC021 Pro)

- UUID: `426C7565-4368-6172-6D42-6561636F6E73` (`BlueCharmBeacons`)
- Georgia (female): MAC `DD:88:00:00:1F:89`, planned Major **3838** Minor **4949**
- Second dog later: Minor **4950**
- Parse: Apple company ID `0x004C`, iBeacon type `0x02`

**0.8.10-ble (collar job):** After STA + HTTP `:8032` + CSI MGMT-only start, the node **auto-starts** a continuous NimBLE observer. No `/ble/start` required. `ble_scan=on` is persisted in NVS. The controller is **left up** (`ble_gap_disc(..., BLE_HS_FOREVER)` at ~16 ms window / 100 ms interval). The 0.8.9 80 ms-on / 8 s-off start/stop/deinit slice is gone — that never left `radio_up` true.

**Wifi/CSI (keep 0.8.9):**

1. **No `csi_collector_enable_data_capture()` at boot** — MGMT-only (RuView#396). DATA promiscuous wedges Core 0 in `wDev_ProcessFiq` / `wifi`.
2. **No `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` / `CONFIG_ESP_WIFI_SW_COEXIST_ENABLE` / `CONFIG_SW_COEXIST_ENABLE`.** Those hung 0.8.5–0.8.8 even with BLE off. 0.8.10 runs NimBLE without them.
3. **Boot order** — STA + `GET /ota/status` first, CSI radio off, then `csi_collector_start()` (MGMT-only), then BLE autostart. CSI callback: no `ESP_LOGI`, no `sendto`, no edge/sync work.

If NVS allow-list is unset, every iBeacon matching the Blue Charm UUID is reported.

Optional keys in `csi_cfg`:

| Key | Type | Meaning |
|-----|------|---------|
| `ble_scan` | u8 | Written by firmware. Missing or `1` = scan on (default). `/ble/stop` writes `0`. |
| `ble_uuid` | 16-byte blob | Override filter UUID (read-only) |
| `ble_allow_mac` | packed 6-byte MACs | MAC allow-list (read-only) |
| `ble_allow_mm` | packed `{u16le major, u16le minor}` | major/minor allow-list (read-only) |

### LAN export

`GET http://<node>:8032/ble/beacons` (no auth):

```json
{"enabled":true,"radio_up":true,"mode":"continuous","window_ms":16,"period_ms":100,"beacons":[{"mac":"dd:88:00:00:1f:89","uuid":"426c7565-4368-6172-6d42-6561636f6e73","major":3838,"minor":4949,"rssi":-62,"last_seen_ms":123456}]}
```

After boot or OTA of this image, with **no extra `/ble/start`**: `enabled` true, `radio_up` true while scanning, nearby Blue Charm iBeacons listed. Ping to the node must stay up.

UDP to `target_ip:target_port` uses magic **`0xB1E00001`** (little-endian). Existing CSI parsers that switch on `0xC511xxxx` ignore it.

## Build (ESP-IDF 5.4.x)

```bash
. $IDF_PATH/export.sh   # or: . ~/esp/esp-idf/export.sh
cd firmware/atom-ble
rm -rf build sdkconfig
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

Host iBeacon parse test (no IDF):

```bash
make -C firmware/atom-ble/tests/host
```

## Release binary (OTA payload)

After a successful build, the OTA-compatible **app image** (load address `0x20000`) is:

| Path | Bytes | Notes |
|------|------:|-------|
| `firmware/atom-ble/release/esp32-csi-node.bin` | see `release/SIZE.txt` | Committed OTA payload (≤ 900000) |
| `firmware/atom-ble/build/esp32-csi-node.bin` | same | IDF output (not committed) |

Version **`0.8.10-ble`**. GitHub Release `v0.8.10-ble` (public repo).

### LAN OTA (0.8.9-ble is live upstairs)

Upstairs `192.168.0.47` is on **0.8.9-ble** (Wi-Fi + LAN OTA work). Flash **0.8.10-ble** over LAN — do not assume USB. 0.8.5–0.8.8 wedge STA (wifi task hang).

On 0.8.6+:

1. After PSK auth, `POST /ota` **immediately** pauses CSI capture/UDP and BLE scan for the write + reboot. Resume only if OTA fails before reboot.
2. The PC should still mute the radio **before** sending the bin when `/csi/stop` exists:

```bash
# Check: csi_control=true and csi_paused on 0.8.6+
curl http://192.168.0.47:8032/ota/status

# Mute CSI+BLE (Bearer PSK; GET or POST)
curl -X POST "http://192.168.0.47:8032/csi/stop" \
  -H "Authorization: Bearer <ota_psk>"

# Stream the app image
curl -X POST "http://192.168.0.47:8032/ota" \
  -H "Authorization: Bearer <ota_psk>" \
  --data-binary @firmware/atom-ble/release/esp32-csi-node.bin
```

Or use the in-repo client (calls `/csi/stop` when `GET /ota/status` reports `csi_control`):

```bash
python3 firmware/atom-ble/ota_client.py 192.168.0.47 \
  firmware/atom-ble/release/esp32-csi-node.bin --psk '<ota_psk>'
```

`GET /ota/status` includes `"csi_paused"` and `"csi_control":true`. `max_size` is 921600.

Do **not** flash bootloader/partition table over USB unless a node is already bricked. OTA only writes the app slot.

## What was cut vs official 0.8.4

WASM3 / Tier 3, LVGL display, mmWave UART, Cognitum swarm HTTP client, adaptive controller, ESP-NOW mesh sync, edge DSP (vitals / HR / pose-adjacent). CSI frames still stream in ADR-018 format (`0xC5110001`).
