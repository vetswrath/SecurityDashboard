/**
 * @file ble_beacon.c
 * @brief iBeacon observer — NimBLE controller OFF at boot.
 *
 * 0.8.5–0.8.7 all hung STA with the BLE controller up (CPU 0 stuck in
 * `wifi`, ICMP/TCP dead). Coexist knobs did not unstick the wifi blob.
 *
 * Exact radio change: ble_beacon_init() does not call nimble_port_init().
 * The BLE controller is not started, so it cannot take the 2.4 GHz radio
 * or block the wifi task. /ble/start (PSK) opts in. Each slice:
 *   nimble_port_init → ~80 ms scan → nimble_port_stop + deinit
 * then 8 s of Wi-Fi-only. Wi-Fi TX fail or OTA hold tears the controller
 * down immediately and leaves it off (TX fail clears the enable flag).
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

#define BLE_SLICE_ON_MS     80
#define BLE_SLICE_OFF_MS    8000
#define BLE_WIFI_FAIL_THRESH 4

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
static SemaphoreHandle_t s_radio_lock;
static volatile bool s_nimble_synced;
static uint8_t s_own_addr_type;
static QueueHandle_t s_udp_q;
static volatile uint32_t s_wifi_fail_streak;

static uint8_t s_filter_uuid[16] = { BLE_BLUECHARM_UUID_BYTES };
static uint8_t s_allow_mac[BLE_ALLOW_MAC_MAX][6];
static uint8_t s_allow_mac_count;
static uint16_t s_allow_major[BLE_ALLOW_MM_MAX];
static uint16_t s_allow_minor[BLE_ALLOW_MM_MAX];
static uint8_t s_allow_mm_count;

/* User asked for scan (/ble/start). Default false — boot never enables this. */
static volatile bool s_user_enabled;
/* OTA /csi/stop hold — keep controller down even if user_enabled. */
static volatile bool s_hold;
/* nimble_port_init has been called and not yet deinit'd. */
static volatile bool s_controller_up;

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
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) {
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
    if (s_udp_q != NULL) {
        (void)xQueueSend(s_udp_q, &copy, 0);
    }
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

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
        ESP_LOGW(TAG, "ble_hs_id_infer_auto rc=%d — using public", rc);
    }
    s_nimble_synced = true;
    ESP_LOGI(TAG, "NimBLE synced (addr_type=%u)", (unsigned)s_own_addr_type);
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

static void controller_down(void)
{
    if (!s_controller_up) {
        return;
    }
    ESP_LOGW(TAG, "stopping NimBLE controller (wifi-only radio)");
    if (s_nimble_synced && ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    (void)nimble_port_stop();
    (void)nimble_port_deinit();
    s_nimble_synced = false;
    s_controller_up = false;
}

static esp_err_t controller_up(void)
{
    if (s_controller_up) {
        return ESP_OK;
    }
    s_nimble_synced = false;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    nimble_port_freertos_init(nimble_host_task);
    s_controller_up = true;

    for (int i = 0; i < 40 && !s_nimble_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_nimble_synced) {
        ESP_LOGE(TAG, "NimBLE sync timeout — tearing controller down");
        controller_down();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static int start_slice_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.itvl = 160;    /* 100 ms */
    params.window = 32;   /* 20 ms inside the 80 ms slice */
    params.passive = 1;
    params.filter_duplicates = 1;
    int rc = ble_gap_disc(s_own_addr_type, BLE_SLICE_ON_MS, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        return rc;
    }
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

/*
 * Hard radio schedule: controller is fully deinitialized except for an
 * ~80 ms scan slice. This is not a coexist knob — the BLE MAC is off.
 */
static void slice_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BLE default OFF — NimBLE controller not started (STA first)");
    while (1) {
        if (!s_user_enabled || s_hold) {
            if (xSemaphoreTake(s_radio_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                controller_down();
                xSemaphoreGive(s_radio_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (xSemaphoreTake(s_radio_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!s_user_enabled || s_hold) {
            controller_down();
            xSemaphoreGive(s_radio_lock);
            continue;
        }
        esp_err_t err = controller_up();
        if (err == ESP_OK) {
            (void)start_slice_scan();
            xSemaphoreGive(s_radio_lock);
            vTaskDelay(pdMS_TO_TICKS(BLE_SLICE_ON_MS + 40));
            if (xSemaphoreTake(s_radio_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
                controller_down();
                xSemaphoreGive(s_radio_lock);
            }
        } else {
            s_user_enabled = false;
            controller_down();
            xSemaphoreGive(s_radio_lock);
            ESP_LOGE(TAG, "BLE controller start failed — leaving BLE off");
            continue;
        }
        /* Wi-Fi-only for several seconds so ICMP/TCP/CSI own the radio. */
        vTaskDelay(pdMS_TO_TICKS(BLE_SLICE_OFF_MS));
    }
}

void ble_beacon_pause(void)
{
    s_hold = true;
    if (s_radio_lock && xSemaphoreTake(s_radio_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        controller_down();
        xSemaphoreGive(s_radio_lock);
    }
    ESP_LOGW(TAG, "BLE hold — controller down");
}

void ble_beacon_resume(void)
{
    s_hold = false;
    ESP_LOGI(TAG, "BLE hold cleared (radio stays down unless /ble/start)");
}

bool ble_beacon_is_paused(void)
{
    return s_hold || !s_user_enabled;
}

esp_err_t ble_beacon_start(void)
{
    s_hold = false;
    s_user_enabled = true;
    s_wifi_fail_streak = 0;
    ESP_LOGW(TAG, "BLE enabled — hard slice %d ms on / %d ms controller-off",
             BLE_SLICE_ON_MS, BLE_SLICE_OFF_MS);
    return ESP_OK;
}

esp_err_t ble_beacon_stop(void)
{
    s_user_enabled = false;
    if (s_radio_lock && xSemaphoreTake(s_radio_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        controller_down();
        xSemaphoreGive(s_radio_lock);
    }
    ESP_LOGW(TAG, "BLE disabled — NimBLE controller off");
    return ESP_OK;
}

bool ble_beacon_is_enabled(void)
{
    return s_user_enabled;
}

bool ble_beacon_radio_is_up(void)
{
    return s_controller_up;
}

void ble_beacon_note_wifi_ok(void)
{
    s_wifi_fail_streak = 0;
}

void ble_beacon_note_wifi_tx_fail(void)
{
    if (!s_user_enabled && !s_controller_up) {
        return;
    }
    s_wifi_fail_streak++;
    if (s_wifi_fail_streak < BLE_WIFI_FAIL_THRESH) {
        return;
    }
    ESP_LOGE(TAG, "Wi-Fi TX failed with BLE requested — disabling BLE for this boot");
    (void)ble_beacon_stop();
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
    cJSON_AddBoolToObject(root, "enabled", s_user_enabled);
    cJSON_AddBoolToObject(root, "radio_up", s_controller_up);
    cJSON *arr = cJSON_AddArrayToObject(root, "beacons");
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
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
        ESP_LOGI(TAG, "GET /ble/beacons registered (no auth); radio off until /ble/start");
    }
    return err;
}

esp_err_t ble_beacon_init(void)
{
    s_user_enabled = false;
    s_hold = false;
    s_controller_up = false;
    s_nimble_synced = false;

    s_lock = xSemaphoreCreateMutex();
    s_radio_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_radio_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_allow_list();

    s_udp_q = xQueueCreate(4, sizeof(ble_sighting_t));
    if (s_udp_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(ble_udp_task, "ble_udp", 3072, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(slice_task, "ble_slice", 4096, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BLE ready but OFF — no nimble_port_init() at boot");
    return ESP_OK;
}
