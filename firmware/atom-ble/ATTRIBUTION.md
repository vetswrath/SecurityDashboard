# Attribution

`firmware/atom-ble` is a local slim fork of:

- Project: [ruvnet/RuView](https://github.com/ruvnet/RuView) `firmware/esp32-csi-node`
- Tag: `v0.8.4-esp32`
- Commit: `c7b3f0bcc5c4091d8e8168f083fca480f7f2cfe9`

Copied into this repository. This is **not** an upstream PR and does not add a second Git remote.

Upstream license: MIT OR Apache-2.0 (see RuView LICENSE).

Kept from 0.8.4: CSI collector (West Wi-Fi, promiscuous CSI, `WIFI_PS_NONE`, UDP to `target_ip:target_port`), NVS `csi_cfg` + `security/ota_psk` reader, HTTP OTA on :8032 fail-closed without PSK (RuView#596), 8MB dual-OTA partition table (`ota_0`/`ota_1` at 0x20000 / 0x220000).

Removed to fit live OTA `max_size` 921600: WASM3 / Tier 3, AMOLED/LVGL, mmWave, swarm HTTP client, adaptive controller, ESP-NOW mesh sync, edge DSP.

Added: GPIO 35 AtomS3 Lite LED, NimBLE iBeacon observer, `GET /ble/beacons`, BLE UDP magic `0xB1E00001`.
