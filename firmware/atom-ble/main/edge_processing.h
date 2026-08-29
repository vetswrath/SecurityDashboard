/**
 * @file edge_processing.h
 * @brief Minimal stub — official RuView Tier-2 DSP is not compiled in.
 *
 * CSI collector still calls edge_enqueue_csi(); this header keeps that
 * call site compiling without WASM/vitals/mmWave.
 */
#ifndef EDGE_PROCESSING_H
#define EDGE_PROCESSING_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define EDGE_VITALS_MAGIC     0xC5110002
#define EDGE_MAX_PERSONS      4
#define EDGE_PHASE_HISTORY_LEN 256
#define EDGE_MAX_SUBCARRIERS  128

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  node_id;
    uint8_t  flags;
    uint16_t breathing_rate;
    uint32_t heartrate;
    int8_t   rssi;
    uint8_t  n_persons;
    uint8_t  reserved[2];
    float    motion_energy;
    float    presence_score;
    uint32_t timestamp_ms;
    uint32_t reserved2;
} edge_vitals_pkt_t;

typedef struct {
    uint8_t  tier;
    float    presence_thresh;
    float    fall_thresh;
    uint16_t vital_window;
    uint16_t vital_interval_ms;
    uint8_t  top_k_count;
    uint8_t  power_duty;
} edge_config_t;

typedef struct {
    float    phase_history[EDGE_PHASE_HISTORY_LEN];
    uint16_t history_len;
    uint16_t history_idx;
    float    breathing_bpm;
    float    heartrate_bpm;
    uint8_t  subcarrier_idx;
    bool     active;
} edge_person_vitals_t;

esp_err_t edge_processing_init(const edge_config_t *cfg);
bool edge_enqueue_csi(const uint8_t *iq_data, uint16_t iq_len,
                      int8_t rssi, uint8_t channel);
bool edge_get_vitals(edge_vitals_pkt_t *pkt);
void edge_get_multi_person(edge_person_vitals_t *persons, uint8_t *n_active);
void edge_get_phase_history(const float **out_buf, uint16_t *out_len,
                            uint16_t *out_idx);
void edge_get_variances(float *out_variances, uint16_t n_subcarriers);

#endif /* EDGE_PROCESSING_H */
