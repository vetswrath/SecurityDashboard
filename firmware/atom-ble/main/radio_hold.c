#include "radio_hold.h"
#include "csi_collector.h"
#include "ble_beacon.h"
#include "esp_log.h"

static const char *TAG = "radio_hold";
static volatile bool s_paused;

void radio_hold_pause(const char *reason)
{
    if (s_paused) {
        return;
    }
    ESP_LOGW(TAG, "pausing CSI+BLE for %s", reason ? reason : "hold");
    csi_collector_pause();
    ble_beacon_pause();
    s_paused = true;
}

void radio_hold_resume(void)
{
    if (!s_paused) {
        return;
    }
    ESP_LOGI(TAG, "resuming CSI+BLE");
    csi_collector_resume();
    ble_beacon_resume();
    s_paused = false;
}

bool radio_hold_is_paused(void)
{
    return s_paused;
}
