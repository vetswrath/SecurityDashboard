/**
 * @file ble_beacon.h
 * @brief iBeacon observer that is OFF unless explicitly started.
 *
 * Boot never calls nimble_port_init(). That is the change that keeps
 * CPU 0 from blocking in `wifi`: the BLE controller is not on the RF.
 * PSK /ble/start time-slices the controller (short on, full deinit off).
 */
#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#define BLE_SIGHTING_MAGIC  0xB1E00001u  /* Distinct from ADR-018 0xC511xxxx */

esp_err_t ble_beacon_init(void);
esp_err_t ble_beacon_register_http(httpd_handle_t server);

/** PSK /ble/start — allow hard-sliced controller windows. */
esp_err_t ble_beacon_start(void);
/** PSK /ble/stop — tear down NimBLE controller and keep it off. */
esp_err_t ble_beacon_stop(void);
bool ble_beacon_is_enabled(void);
bool ble_beacon_radio_is_up(void);

void ble_beacon_pause(void);
void ble_beacon_resume(void);
bool ble_beacon_is_paused(void);

void ble_beacon_note_wifi_ok(void);
/** Wi-Fi TX failed while BLE was requested — disable BLE for the rest of boot. */
void ble_beacon_note_wifi_tx_fail(void);

#endif /* BLE_BEACON_H */
