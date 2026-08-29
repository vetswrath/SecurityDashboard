# SecurityDashboard

Home CSI + collar-tag nodes for Thomas West’s house.

## Firmware

AtomS3 Lite CSI nodes with HTTP OTA and BLE iBeacon scan for Blue Charm dog-collar tags:

**[`firmware/atom-ble/`](firmware/atom-ble/)**

- Board: M5Stack AtomS3 Lite (ESP32-S3, 8MB), LED GPIO 35
- Live OTA cap: **921600** bytes (`POST :8032/ota`). New app `.bin` must be **≤ 900000**
- Release payload: `firmware/atom-ble/release/esp32-csi-node.bin` (app at `0x20000`, version **0.8.10-ble**)
- 0.8.10: auto-start continuous low-duty NimBLE scan after CSI MGMT-only; persist `ble_scan=on`. Same 0.8.9 wifi/CSI: MGMT-only, no SW coexist flags.
- LAN OTA: if `GET :8032/ota/status` reports `csi_control`, call `POST :8032/csi/stop` (Bearer PSK) **before** `POST /ota`. See `firmware/atom-ble/ota_client.py`.
- See [`firmware/atom-ble/README.md`](firmware/atom-ble/README.md) for build and OTA notes

This firmware is a local slim copy of [ruvnet/RuView](https://github.com/ruvnet/RuView) `firmware/esp32-csi-node` v0.8.4. It is not an upstream PR.
