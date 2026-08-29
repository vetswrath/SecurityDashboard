/**
 * @file ble_beacon.h
 * @brief Continuous low-duty NimBLE iBeacon observer.
 *
 * After STA + HTTP + CSI MGMT-only are up, ble_beacon_autostart() brings
 * the controller up once and leaves it up. Scan is BLE_HS_FOREVER at
 * ~16 ms / 100 ms. No start/stop/deinit slice. ble_scan=on is persisted
 * in NVS so a reboot does not require /ble/start.
 */
#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#define BLE_SIGHTING_MAGIC  0xB1E00001u  /* Distinct from ADR-018 0xC511xxxx */

esp_err_t ble_beacon_init(void);
esp_err_t ble_beacon_register_http(httpd_handle_t server);

/** Read NVS ble_scan (default on), persist it, start continuous scan. */
esp_err_t ble_beacon_autostart(void);

/** PSK /ble/start — enable + persist + continuous scan (controller stays up). */
esp_err_t ble_beacon_start(void);
/** PSK /ble/stop — persist off and tear down NimBLE. */
esp_err_t ble_beacon_stop(void);
bool ble_beacon_is_enabled(void);
bool ble_beacon_radio_is_up(void);

void ble_beacon_pause(void);
void ble_beacon_resume(void);
bool ble_beacon_is_paused(void);

void ble_beacon_note_wifi_ok(void);
/** Wi-Fi TX failed — brief scan cancel, controller stays up, scan resumes. */
void ble_beacon_note_wifi_tx_fail(void);

#endif /* BLE_BEACON_H */
