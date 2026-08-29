/**
 * @file ble_beacon.c
 * @brief Time-sliced NimBLE iBeacon observer for Blue Charm collar tags.
 *
 * Coexistence: ESP-IDF software Wi-Fi/BLE coexistence + short observer
 * windows (default 300 ms every 2.5 s). Wi-Fi stays on WIFI_PS_NONE so CSI
 * keeps running; fps may dip during a scan window.
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
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "sdkconfig.h"

static const char *TAG = "ble_beacon";

#ifndef CONFIG_BLE_SCAN_WINDOW_MS
#define CONFIG_BLE_SCAN_WINDOW_MS 300
#endif
#ifndef CONFIG_BLE_SCAN_PERIOD_MS
#define CONFIG_BLE_SCAN_PERIOD_MS 2500
#endif

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

static uint8_t s_filter_uuid[16] = { BLE_BLUECHARM_UUID_BYTES };
static uint8_t s_allow_mac[BLE_ALLOW_MAC_MAX][6];
static uint8_t s_allow_mac_count;
static uint16_t s_allow_major[BLE_ALLOW_MM_MAX];
static uint16_t s_allow_minor[BLE_ALLOW_MM_MAX];
static uint8_t s_allow_mm_count;

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
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
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
    send_udp_sighting(&copy);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
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

static void start_scan_window(void)
{
    struct ble_gap_disc_params params = {0};
    params.itvl = 48;     /* 30 ms */
    params.window = 48;
    params.passive = 1;
    params.filter_duplicates = 0;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, CONFIG_BLE_SCAN_WINDOW_MS,
                          &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

static void scan_task(void *arg)
{
    (void)arg;
    while (!s_nimble_synced) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "BLE time-slice: %d ms window / %d ms period (CSI may dip in window)",
             CONFIG_BLE_SCAN_WINDOW_MS, CONFIG_BLE_SCAN_PERIOD_MS);
    const int idle_ms = CONFIG_BLE_SCAN_PERIOD_MS - CONFIG_BLE_SCAN_WINDOW_MS;
    while (1) {
        start_scan_window();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BLE_SCAN_WINDOW_MS + 20));
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
        }
        if (idle_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(idle_ms));
        }
    }
}

static void on_sync(void)
{
    s_nimble_synced = true;
    ESP_LOGI(TAG, "NimBLE synced — observer ready");
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

    if (xTaskCreate(scan_task, "ble_scan", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
