/**
 * @file c6_sync_espnow.h
 * @brief No-op ESP-NOW sync stubs (atom-ble does not run mesh sync).
 */
#pragma once

#include "esp_err.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>

static inline esp_err_t c6_sync_espnow_init(void) { return ESP_OK; }
static inline uint64_t  c6_sync_espnow_get_epoch_us(void)
{
    return (uint64_t)esp_timer_get_time();
}
static inline bool    c6_sync_espnow_is_leader(void) { return false; }
static inline bool    c6_sync_espnow_is_valid(void) { return false; }
static inline int64_t c6_sync_espnow_get_offset_us(void) { return 0; }
static inline int64_t c6_sync_espnow_get_offset_us_smoothed(void) { return 0; }
static inline uint32_t c6_sync_espnow_tx_count(void) { return 0; }
static inline uint32_t c6_sync_espnow_tx_fail(void) { return 0; }
static inline uint32_t c6_sync_espnow_rx_count(void) { return 0; }
static inline uint32_t c6_sync_espnow_rx_magic_match(void) { return 0; }
