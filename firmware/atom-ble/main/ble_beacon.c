/**
 * @file ble_beacon.c
 * @brief Low-duty NimBLE iBeacon observer for Blue Charm collar tags.
 *
 * 0.8.5/0.8.6 wedged STA: ble_gap_disc ran 300 ms at 100% duty (itvl==window)
 * every 2.5 s on CPU 0 next to the Wi-Fi task. CSI sendto from the Wi-Fi
 * callback then blocked on RF. Symptom: ping_sock send error=0, ICMP/TCP
 * dead, task_wdt IDLE0 / CPU0 stuck in wifi.
 *
 * Coexist (this file + sdkconfig.defaults + main.c):
 *   1. Continuous passive scan at ~16 ms RX / 100 ms interval (~16% BLE).
 *      Do not start/stop 300 ms 100% bursts (RF calib wedges wifi).
 *   2. NimBLE host + controller pinned to CPU 1; Wi-Fi stays CPU 0.
 *   3. GAP callback never sendto — UDP is a separate task.
 *   4. If Wi-Fi TX fails, cancel scan for 8 s, then resume.
 *   5. main.c sets ESP_COEX_PREFER_WIFI.
 */
#include "ble_beacon.h"
#include "ble_ibeacon.h"
#include "nvs_config.h"
#include "csi_collector.h"
#include "stream_sender.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_gap.h"
#include "sdkconfig.h"

static const char *TAG = "ble_beacon";

#ifndef CONFIG_BLE_SCAN_WINDOW_MS
#define CONFIG_BLE_SCAN_WINDOW_MS 16
#endif
#ifndef CONFIG_BLE_SCAN_PERIOD_MS
#define CONFIG_BLE_SCAN_PERIOD_MS 100
#endif

/* Hard cap: 0.8.5/0.8.6 Kconfig was 300/2500 at 100% duty and hung STA. */
#define BLE_RF_WINDOW_MAX_MS    24
#define BLE_RF_INTERVAL_MIN_MS  80
#define BLE_RF_INTERVAL_MAX_MS  200
#define BLE_WIFI_FAIL_BACKOFF_US (8LL * 1000 * 1000)
#define BLE_WIFI_FAIL_THRESH    8

#define BLE_SIGHTING_MAX      8
#define BLE_ALLOW_MAC_MAX     8
#define BLE_ALLOW_MM_MAX      8

extern nvs_config_t g_nvs_config;

typedef struct {
    uint8_t  mac[6];
    uint8_t  uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t   rssi;
    uint32_t last_seen_ms;
    uint8_t  used;
} ble_sighting_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  node_id;
    uint8_t  reserved;
    uint16_t major;
    uint16_t minor;
    int8_t   rssi;
    uint8_t  pad;
    uint8_t  mac[6];
    uint8_t  uuid[16];
    uint32_t last_seen_ms;
} ble_udp_pkt_t;
#pragma pack(pop)

static ble_sighting_t s_sight[BLE_SIGHTING_MAX];
static SemaphoreHandle_t s_lock;
static volatile bool s_nimble_synced;
static uint8_t s_own_addr_type;
static QueueHandle_t s_udp_q;
static volatile int64_t s_backoff_until_us;
static volatile uint32_t s_wifi_fail_streak;
static volatile bool s_need_restart;

static uint8_t s_filter_uuid[16] = { BLE_BLUECHARM_UUID_BYTES };
static uint8_t s_allow_mac[BLE_ALLOW_MAC_MAX][6];
static uint8_t s_allow_mac_count;
static uint16_t s_allow_major[BLE_ALLOW_MM_MAX];
static uint16_t s_allow_minor[BLE_ALLOW_MM_MAX];
static uint8_t s_allow_mm_count;
static volatile bool s_scan_paused;

static void load_allow_list(void)
{
    nvs_handle_t nvs;
    if (nvs_open("csi_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No csi_cfg NVS — report all Blue Charm UUID iBeacons");
        return;
    }

    size_t len = sizeof(s_filter_uuid);
    if (nvs_get_blob(nvs, "ble_uuid", s_filter_uuid, &len) == ESP_OK && len == 16) {
        ESP_LOGI(TAG, "NVS ble_uuid override loaded");
    }

    uint8_t mac_blob[BLE_ALLOW_MAC_MAX * 6];
    len = sizeof(mac_blob);
    if (nvs_get_blob(nvs, "ble_allow_mac", mac_blob, &len) == ESP_OK && len >= 6) {
        s_allow_mac_count = (uint8_t)(len / 6);
        if (s_allow_mac_count > BLE_ALLOW_MAC_MAX) {
            s_allow_mac_count = BLE_ALLOW_MAC_MAX;
        }
        memcpy(s_allow_mac, mac_blob, (size_t)s_allow_mac_count * 6);
        ESP_LOGI(TAG, "NVS ble_allow_mac: %u entries", (unsigned)s_allow_mac_count);
    }

    uint8_t mm_blob[BLE_ALLOW_MM_MAX * 4];
    len = sizeof(mm_blob);
    if (nvs_get_blob(nvs, "ble_allow_mm", mm_blob, &len) == ESP_OK && len >= 4) {
        s_allow_mm_count = (uint8_t)(len / 4);
        if (s_allow_mm_count > BLE_ALLOW_MM_MAX) {
            s_allow_mm_count = BLE_ALLOW_MM_MAX;
        }
        for (uint8_t i = 0; i < s_allow_mm_count; i++) {
            s_allow_major[i] = (uint16_t)mm_blob[i * 4] | ((uint16_t)mm_blob[i * 4 + 1] << 8);
            s_allow_minor[i] = (uint16_t)mm_blob[i * 4 + 2] | ((uint16_t)mm_blob[i * 4 + 3] << 8);
        }
        ESP_LOGI(TAG, "NVS ble_allow_mm: %u entries", (unsigned)s_allow_mm_count);
    }

    nvs_close(nvs);
    if (s_allow_mac_count == 0 && s_allow_mm_count == 0) {
        ESP_LOGI(TAG, "BLE allow-list unset — all matching-UUID iBeacons reported");
    }
}

static bool uuid_matches_filter(const uint8_t uuid[16])
{
    return memcmp(uuid, s_filter_uuid, 16) == 0;
}

static bool allow_list_ok(const uint8_t mac[6], uint16_t major, uint16_t minor)
{
    if (s_allow_mac_count == 0 && s_allow_mm_count == 0) {
        return true;
    }
    for (uint8_t i = 0; i < s_allow_mac_count; i++) {
        if (memcmp(mac, s_allow_mac[i], 6) == 0) {
            return true;
        }
    }
    for (uint8_t i = 0; i < s_allow_mm_count; i++) {
        if (s_allow_major[i] == major && s_allow_minor[i] == minor) {
            return true;
        }
    }
    return false;
}

static void send_udp_sighting(const ble_sighting_t *s)
{
    ble_udp_pkt_t pkt = {0};
    pkt.magic = BLE_SIGHTING_MAGIC;
    pkt.node_id = csi_collector_get_node_id();
    pkt.major = s->major;
    pkt.minor = s->minor;
    pkt.rssi = s->rssi;
    memcpy(pkt.mac, s->mac, 6);
    memcpy(pkt.uuid, s->uuid, 16);
    pkt.last_seen_ms = s->last_seen_ms;
    stream_sender_send_priority((const uint8_t *)&pkt, sizeof(pkt));
}

static void record_sighting(const uint8_t mac[6], const ble_ibeacon_t *ib, int8_t rssi)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    int slot = -1;
    int empty = -1;
    for (int i = 0; i < BLE_SIGHTING_MAX; i++) {
        if (s_sight[i].used && memcmp(s_sight[i].mac, mac, 6) == 0) {
            slot = i;
            break;
        }
        if (!s_sight[i].used && empty < 0) {
            empty = i;
        }
    }
    if (slot < 0) {
        slot = (empty >= 0) ? empty : 0;
    }
    memcpy(s_sight[slot].mac, mac, 6);
    memcpy(s_sight[slot].uuid, ib->uuid, 16);
    s_sight[slot].major = ib->major;
    s_sight[slot].minor = ib->minor;
    s_sight[slot].rssi = rssi;
    s_sight[slot].last_seen_ms = now_ms;
    s_sight[slot].used = 1;
    ble_sighting_t copy = s_sight[slot];
    xSemaphoreGive(s_lock);
    /* Never sendto from the NimBLE host / GAP callback — that deadlocks
     * against the Wi-Fi task when BLE holds the 2.4 GHz radio. */
    if (s_udp_q != NULL) {
        (void)xQueueSend(s_udp_q, &copy, 0);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_need_restart = true;
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    ble_ibeacon_t ib;
    if (!ble_ibeacon_parse(event->disc.data, event->disc.length_data, &ib)) {
        return 0;
    }
    if (!uuid_matches_filter(ib.uuid)) {
        return 0;
    }
    uint8_t mac[6];
    memcpy(mac, event->disc.addr.val, 6);
    if (!allow_list_ok(mac, ib.major, ib.minor)) {
        return 0;
    }
    record_sighting(mac, &ib, (int8_t)event->disc.rssi);
    return 0;
}

static void ble_rf_duty_ms(int *window_ms, int *interval_ms)
{
    int win = CONFIG_BLE_SCAN_WINDOW_MS;
    int ivl = CONFIG_BLE_SCAN_PERIOD_MS;
    if (win > BLE_RF_WINDOW_MAX_MS) {
        win = 16;
    }
    if (ivl < BLE_RF_INTERVAL_MIN_MS || ivl > BLE_RF_INTERVAL_MAX_MS) {
        ivl = 100;
    }
    if (win >= ivl) {
        win = ivl / 6;
        if (win < 8) {
            win = 8;
        }
    }
    *window_ms = win;
    *interval_ms = ivl;
}

static int start_low_duty_scan(void)
{
    int win_ms, ivl_ms;
    ble_rf_duty_ms(&win_ms, &ivl_ms);

    struct ble_gap_disc_params params = {0};
    /* NimBLE units are 0.625 ms. window << interval so Wi-Fi keeps the RF. */
    params.itvl = (uint16_t)((ivl_ms * 8) / 5);
    params.window = (uint16_t)((win_ms * 8) / 5);
    if (params.window < 16) {
        params.window = 16; /* 10 ms */
    }
    if (params.itvl <= params.window) {
        params.itvl = (uint16_t)(params.window * 6);
    }
    params.passive = 1;
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "BLE continuous scan %u/%u units (~%d ms / %d ms, prefer Wi-Fi)",
             (unsigned)params.window, (unsigned)params.itvl, win_ms, ivl_ms);
    return 0;
}

static void ble_udp_task(void *arg)
{
    (void)arg;
    ble_sighting_t s;
    while (xQueueReceive(s_udp_q, &s, portMAX_DELAY) == pdTRUE) {
        send_udp_sighting(&s);
    }
}

static void scan_task(void *arg)
{
    (void)arg;
    while (!s_nimble_synced) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* Let STA, ping_sock, and :8032 come up before the BLE controller
     * touches the radio. /ota/status must answer within ~15 s of boot. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (!s_scan_paused) {
        start_low_duty_scan();
    }

    while (1) {
        int64_t now = esp_timer_get_time();
        bool backoff = (s_backoff_until_us > 0 && now < s_backoff_until_us);
        if (s_scan_paused || backoff) {
            if (ble_gap_disc_active()) {
                ble_gap_disc_cancel();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (s_backoff_until_us > 0 && now >= s_backoff_until_us) {
            s_backoff_until_us = 0;
            s_need_restart = true;
            ESP_LOGI(TAG, "BLE scan backoff done — restarting low-duty scan");
        }
        if (s_need_restart || !ble_gap_disc_active()) {
            s_need_restart = false;
            start_low_duty_scan();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ble_beacon_pause(void)
{
    s_scan_paused = true;
    if (s_nimble_synced && ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    ESP_LOGW(TAG, "BLE scan paused");
}

void ble_beacon_resume(void)
{
    s_scan_paused = false;
    s_need_restart = true;
    ESP_LOGI(TAG, "BLE scan resumed");
}

bool ble_beacon_is_paused(void)
{
    return s_scan_paused;
}

void ble_beacon_note_wifi_ok(void)
{
    s_wifi_fail_streak = 0;
}

void ble_beacon_note_wifi_tx_fail(void)
{
    if (s_scan_paused) {
        return;
    }
    s_wifi_fail_streak++;
    if (s_wifi_fail_streak < BLE_WIFI_FAIL_THRESH) {
        return;
    }
    s_wifi_fail_streak = 0;
    s_backoff_until_us = esp_timer_get_time() + BLE_WIFI_FAIL_BACKOFF_US;
    if (s_nimble_synced && ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    ESP_LOGW(TAG, "Wi-Fi TX failing — BLE scan backoff 8s so STA can recover");
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
        ESP_LOGW(TAG, "ble_hs_id_infer_auto rc=%d — using public", rc);
    }
    s_nimble_synced = true;
    ESP_LOGI(TAG, "NimBLE synced — observer ready (addr_type=%u)",
             (unsigned)s_own_addr_type);
}

static void on_reset(int reason)
{
    s_nimble_synced = false;
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void uuid_to_str(const uint8_t u[16], char *out, size_t out_len)
{
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void mac_to_str(const uint8_t m[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static esp_err_t ble_beacons_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "beacons");
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < BLE_SIGHTING_MAX; i++) {
            if (!s_sight[i].used) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            char mac[18];
            char uuid[40];
            mac_to_str(s_sight[i].mac, mac, sizeof(mac));
            uuid_to_str(s_sight[i].uuid, uuid, sizeof(uuid));
            cJSON_AddStringToObject(o, "mac", mac);
            cJSON_AddStringToObject(o, "uuid", uuid);
            cJSON_AddNumberToObject(o, "major", s_sight[i].major);
            cJSON_AddNumberToObject(o, "minor", s_sight[i].minor);
            cJSON_AddNumberToObject(o, "rssi", s_sight[i].rssi);
            cJSON_AddNumberToObject(o, "last_seen_ms", (double)s_sight[i].last_seen_ms);
            cJSON_AddItemToArray(arr, o);
        }
        xSemaphoreGive(s_lock);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, (ssize_t)strlen(json));
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t ble_beacon_register_http(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t uri = {
        .uri = "/ble/beacons",
        .method = HTTP_GET,
        .handler = ble_beacons_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GET /ble/beacons registered (no auth)");
    }
    return err;
}

esp_err_t ble_beacon_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_allow_list();

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    nimble_port_freertos_init(nimble_host_task);

    s_udp_q = xQueueCreate(4, sizeof(ble_sighting_t));
    if (s_udp_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* CPU 1: keep NimBLE + BLE UDP off the Wi-Fi core (CPU 0). */
    if (xTaskCreatePinnedToCore(ble_udp_task, "ble_udp", 3072, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(scan_task, "ble_scan", 3072, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
