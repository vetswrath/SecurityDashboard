/**
 * @file edge_processing_stub.c
 * @brief No-op stand-in for RuView ADR-039 Tier-2 DSP.
 *
 * Official 0.8.4 edge_processing.c + WASM3 is why the upstream app.bin is
 * ~1.13 MB and fails live OTA (max_size 921600). This stub keeps the CSI
 * collector call sites compiling. No pose or medical output is produced.
 */
#include "edge_processing.h"
#include "esp_log.h"

static const char *TAG = "edge_stub";

esp_err_t edge_processing_init(const edge_config_t *cfg)
{
    (void)cfg;
    ESP_LOGI(TAG, "Tier-2 DSP not compiled in (atom-ble slim build)");
    return ESP_OK;
}

bool edge_enqueue_csi(const uint8_t *iq_data, uint16_t iq_len,
                      int8_t rssi, uint8_t channel)
{
    (void)iq_data;
    (void)iq_len;
    (void)rssi;
    (void)channel;
    return true;
}

bool edge_get_vitals(edge_vitals_pkt_t *pkt)
{
    if (pkt) {
        *pkt = (edge_vitals_pkt_t){0};
    }
    return false;
}

void edge_get_multi_person(edge_person_vitals_t *persons, uint8_t *n_active)
{
    if (n_active) {
        *n_active = 0;
    }
    (void)persons;
}

void edge_get_phase_history(const float **out_buf, uint16_t *out_len,
                            uint16_t *out_idx)
{
    if (out_buf) {
        *out_buf = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out_idx) {
        *out_idx = 0;
    }
}

void edge_get_variances(float *out_variances, uint16_t n_subcarriers)
{
    if (out_variances && n_subcarriers) {
        for (uint16_t i = 0; i < n_subcarriers; i++) {
            out_variances[i] = 0.0f;
        }
    }
}
