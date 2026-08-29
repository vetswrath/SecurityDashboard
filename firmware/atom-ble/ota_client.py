#!/usr/bin/env python3
"""Push an OTA app image to an AtomS3 Lite CSI node.

If GET /ota/status reports csi_control (0.8.6-ble+), mute CSI+BLE with
/csi/stop before POST /ota so the 2.4 GHz radio can finish the upload
in about a minute. Live 0.8.4 has no /csi/stop — POST /ota alone will
time out while CSI UDP is running. Live upstairs is 0.8.9-ble (STA+OTA
work). Push 0.8.10-ble over LAN. 0.8.5–0.8.8 wedge STA.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def req(method: str, url: str, *, data: bytes | None = None,
        headers: dict[str, str] | None = None, timeout: float = 30) -> tuple[int, bytes]:
    r = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def main() -> int:
    p = argparse.ArgumentParser(description="OTA push with CSI mute when available")
    p.add_argument("host", help="Node IP, e.g. 192.168.0.47")
    p.add_argument("bin", help="Path to esp32-csi-node.bin")
    p.add_argument("--psk", required=True, help="NVS security/ota_psk")
    p.add_argument("--port", type=int, default=8032)
    args = p.parse_args()

    base = f"http://{args.host}:{args.port}"
    auth = {"Authorization": f"Bearer {args.psk}"}

    code, body = req("GET", f"{base}/ota/status", timeout=10)
    print(f"GET /ota/status -> {code} {body.decode('utf-8', 'replace')}")
    if code != 200:
        print("status failed; abort", file=sys.stderr)
        return 1
    meta = json.loads(body.decode())
    has_csi_ctrl = bool(meta.get("csi_control")) or meta.get("csi_paused") is not None

    if has_csi_ctrl:
        print("Calling POST /csi/stop (mute CSI+BLE before upload)")
        code, body = req("POST", f"{base}/csi/stop", headers=auth, timeout=10)
        print(f"POST /csi/stop -> {code} {body.decode('utf-8', 'replace')}")
        if code != 200:
            print("csi/stop failed; abort (fail-closed)", file=sys.stderr)
            return 1
        time.sleep(0.3)
    else:
        print("No /csi/stop on this image (0.8.4). USB-flash 0.8.6-ble once.")

    payload = open(args.bin, "rb").read()
    print(f"POST /ota {len(payload)} bytes ...")
    hdrs = dict(auth)
    hdrs["Content-Type"] = "application/octet-stream"
    t0 = time.time()
    code, body = req("POST", f"{base}/ota", data=payload, headers=hdrs, timeout=90)
    print(f"POST /ota -> {code} in {time.time() - t0:.1f}s {body.decode('utf-8', 'replace')}")
    return 0 if code == 200 else 1


if __name__ == "__main__":
    raise SystemExit(main())
