# AtomS3 Lite CSI + BLE firmware (`atom-ble`)

House CSI nodes (M5Stack AtomS3 Lite, ESP32-S3, 8MB) that keep live CSI + HTTP OTA and add time-sliced BLE iBeacon scanning for Blue Charm dog-collar tags.

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
| OTA fail-closed | No PSK in NVS `security/ota_psk` → POST rejected (RuView#596) |
| Partitions | Same 8MB dual-OTA table as live (`partitions.csv`, app at `0x20000`) |
| NVS | Reads existing `csi_cfg` + `security`. Does not provision/wipe keys. |

Tonight’s OTA targets: node 1 upstairs-living `192.168.0.47`, node 2 stairs `192.168.0.49`.

## BLE (Blue Charm BC021 Pro)

- UUID: `426C7565-4368-6172-6D42-6561636F6E73` (`BlueCharmBeacons`)
- Georgia (female): MAC `DD:88:00:00:1F:89`, planned Major **3838** Minor **4949**
- Second dog later: Minor **4950**
- Parse: Apple company ID `0x004C`, iBeacon type `0x02`

**Coexistence:** ESP-IDF software Wi-Fi/BLE coexistence (`CONFIG_SW_COEXIST_ENABLE`) plus **time-sliced NimBLE observer** — default **300 ms scan every 2.5 s**. Wi-Fi stays `WIFI_PS_NONE`. CSI fps may dip during a scan window.

If NVS allow-list is unset, every iBeacon matching the Blue Charm UUID is reported.

Optional read-only keys in `csi_cfg` (never written by firmware):

| Key | Type | Meaning |
|-----|------|---------|
| `ble_uuid` | 16-byte blob | Override filter UUID |
| `ble_allow_mac` | packed 6-byte MACs | MAC allow-list |
| `ble_allow_mm` | packed `{u16le major, u16le minor}` | major/minor allow-list |

### LAN export

`GET http://<node>:8032/ble/beacons` (no auth):

```json
{"beacons":[{"mac":"dd:88:00:00:1f:89","uuid":"426c7565-4368-6172-6d42-6561636f6e73","major":3838,"minor":4949,"rssi":-62,"last_seen_ms":123456}]}
```

UDP to `target_ip:target_port` uses magic **`0xB1E00001`** (little-endian). Existing CSI parsers that switch on `0xC511xxxx` ignore it.

## Build (ESP-IDF 5.4.x)

```bash
. $IDF_PATH/export.sh   # or: . ~/esp/esp-idf/export.sh
cd firmware/atom-ble
rm -rf build sdkconfig
idf.py set-target esp32s3
idf.py build
```

Host iBeacon parse test (no IDF):

```bash
make -C firmware/atom-ble/tests/host
```

## Release binary (OTA payload)

After a successful build, the OTA-compatible **app image** (load address `0x20000`) is:

| Path | What |
|------|------|
| `firmware/atom-ble/build/esp32-csi-node.bin` | IDF output |
| `firmware/atom-ble/release/esp32-csi-node.bin` | Copied release (committed when size ≤ 900000) |

Push:

```bash
curl -X POST "http://192.168.0.47:8032/ota" \
  -H "Authorization: Bearer <ota_psk>" \
  --data-binary @firmware/atom-ble/release/esp32-csi-node.bin
```

Check first: `curl http://192.168.0.47:8032/ota/status` — `max_size` is 921600.

Do **not** flash bootloader/partition table over USB unless a node is already bricked. OTA only writes the app slot.

## What was cut vs official 0.8.4

WASM3 / Tier 3, LVGL display, mmWave UART, Cognitum swarm HTTP client, adaptive controller, ESP-NOW mesh sync, edge DSP (vitals / HR / pose-adjacent). CSI frames still stream in ADR-018 format (`0xC5110001`).
