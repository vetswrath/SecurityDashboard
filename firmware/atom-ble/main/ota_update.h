/**
 * @file ota_update.h
 * @brief HTTP OTA firmware update endpoint for ESP32-S3 CSI Node.
 *
 * HTTP on port 8032:
 *   GET  /ota/status      — version, partitions, max_size, csi_paused, csi_control
 *   POST /ota             — app image (Bearer PSK). Pauses CSI+BLE after auth.
 *   GET/POST /csi/stop    — pause CSI UDP + BLE scan (Bearer PSK)
 *   GET/POST /csi/start   — resume (Bearer PSK). OTA success reboots instead.
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"

/**
 * Initialize the OTA update HTTP server.
 * Starts a lightweight HTTP server on port 8032 that accepts
 * POST /ota with a firmware binary payload.
 *
 * @return ESP_OK on success.
 */
esp_err_t ota_update_init(void);

/**
 * Initialize the OTA update HTTP server and return the handle.
 * Same as ota_update_init() but exposes the httpd_handle_t so
 * other modules (e.g. WASM upload) can register additional endpoints.
 *
 * @param out_server  Output: HTTP server handle (may be NULL on failure).
 * @return ESP_OK on success.
 */
esp_err_t ota_update_init_ex(void **out_server);

#endif /* OTA_UPDATE_H */
