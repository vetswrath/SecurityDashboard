/**
 * @file ble_beacon.h
 * @brief Low-duty NimBLE iBeacon observer + LAN sighting export.
 *
 * Scan is continuous at ~16 ms BLE RX / 100 ms (not 300 ms 100% bursts).
 * NimBLE is pinned to CPU 1 so it cannot wedge the Wi-Fi task on CPU 0.
 */
#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#define BLE_SIGHTING_MAGIC  0xB1E00001u  /* Distinct from ADR-018 0xC511xxxx */

esp_err_t ble_beacon_init(void);
esp_err_t ble_beacon_register_http(httpd_handle_t server);

void ble_beacon_pause(void);
void ble_beacon_resume(void);
bool ble_beacon_is_paused(void);

/** Wi-Fi TX recovered — clear BLE backoff streak. */
void ble_beacon_note_wifi_ok(void);

/** Wi-Fi TX failed (sendto/ping). After several hits, pause BLE scan. */
void ble_beacon_note_wifi_tx_fail(void);

#endif /* BLE_BEACON_H */
