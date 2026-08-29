/**
 * @file main.c
 * @brief AtomS3 Lite CSI node + continuous low-duty BLE iBeacon scan.
 *
 * Slim fork of ruvnet/RuView firmware/esp32-csi-node v0.8.4:
 * CSI collector, UDP stream, NVS config, HTTP OTA, GPIO 35 LED.
 * WASM / display / mmWave / swarm / adaptive controller are not linked.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

#include "csi_collector.h"
#include "stream_sender.h"
#include "nvs_config.h"
#include "ota_update.h"
#include "power_mgmt.h"
#include "led_status.h"
#include "ble_beacon.h"

static const char *TAG = "main";

nvs_config_t g_nvs_config;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define MAX_RETRY 10

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d rssi=%d", disc->reason, disc->rssi);
        led_status_set_online(false);
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        led_status_set_online(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            /* WPA_PSK so WPA/WPA2-mixed routers are not rejected (#1050). */
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, g_nvs_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_nvs_config.wifi_password, sizeof(wifi_config.sta.password) - 1);

    if (strlen((char *)wifi_config.sta.password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized, connecting to SSID: %s", g_nvs_config.wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after %d retries", MAX_RETRY);
    }
}

void app_main(void)
{
    /* Same NVS init as RuView 0.8.4. Erase only on corrupt/new-version —
     * do not change partition size or namespace layout. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_config_load(&g_nvs_config);
    csi_collector_set_node_id(g_nvs_config.node_id);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "AtomS3 Lite CSI+BLE node v%s — Node ID: %d zone=%s",
             app_desc->version, g_nvs_config.node_id, g_nvs_config.zone_name);

    (void)led_status_init();

    wifi_init_sta();

    if (stream_sender_init_with(g_nvs_config.target_ip, g_nvs_config.target_port) != 0) {
        ESP_LOGE(TAG, "Failed to initialize UDP sender");
        return;
    }

    /* 0.8.4 atom-led: HTTP on :8032 while CSI is MGMT-only. This tree used
     * to enable CSI + MGMT+DATA before HTTP — that wedges wifi. Prove STA
     * + OTA first, then turn CSI on (MGMT-only, no #893 DATA upgrade). */
    csi_collector_init();

    httpd_handle_t ota_server = NULL;
    esp_err_t ota_ret = ota_update_init_ex((void **)&ota_server);
    if (ota_ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA server init failed: %s", esp_err_to_name(ota_ret));
    }

    esp_err_t ble_ret = ble_beacon_init();
    if (ble_ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ble_ret));
    } else if (ota_server != NULL) {
        ble_beacon_register_http(ota_server);
    }

    power_mgmt_init(g_nvs_config.power_duty);

    ESP_LOGI(TAG, "STA+HTTP up — delaying CSI so ping/:8032 can answer");
    vTaskDelay(pdMS_TO_TICKS(2000));

    csi_collector_start();
    if (g_nvs_config.channel_hop_count > 1) {
        ESP_LOGI(TAG, "Starting channel hopping: %u channels, dwell=%lu ms",
                 (unsigned)g_nvs_config.channel_hop_count,
                 (unsigned long)g_nvs_config.dwell_ms);
        csi_collector_set_hop_table(
            g_nvs_config.channel_list,
            g_nvs_config.channel_hop_count,
            g_nvs_config.dwell_ms);
    }

    /* Collar job: start NimBLE after CSI MGMT-only is running. No PSK
     * /ble/start required. Persist ble_scan=on. Controller stays up. */
    if (ble_ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        (void)ble_beacon_autostart();
    }

    ESP_LOGI(TAG, "CSI MGMT-only → %s:%d (OTA=%s, BLE=%s, LED=GPIO35)",
             g_nvs_config.target_ip, g_nvs_config.target_port,
             (ota_ret == ESP_OK) ? "ready" : "off",
             (ble_ret == ESP_OK) ? "auto-scan" : "init-fail");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
