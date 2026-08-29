set(SRCS
    "main.c"
    "csi_collector.c"
    "stream_sender.c"
    "nvs_config.c"
    "ota_update.c"
    "power_mgmt.c"
    "led_status.c"
    "ble_beacon.c"
    "ble_ibeacon.c"
    "edge_processing_stub.c"
    "c6_sync_espnow_stub.c"
)

set(REQUIRES
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    app_update
    esp_http_server
    esp_app_format
    esp_timer
    esp_pm
    esp_driver_gpio
    driver
    lwip
    bt
    json
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
