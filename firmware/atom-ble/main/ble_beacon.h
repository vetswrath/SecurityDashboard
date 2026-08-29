/**
 * @file ble_beacon.h
 * @brief Time-sliced NimBLE iBeacon observer + LAN sighting export.
 */
#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "esp_err.h"
#include "esp_http_server.h"

#define BLE_SIGHTING_MAGIC  0xB1E00001u  /* Distinct from ADR-018 0xC511xxxx */

esp_err_t ble_beacon_init(void);
esp_err_t ble_beacon_register_http(httpd_handle_t server);

#endif /* BLE_BEACON_H */
