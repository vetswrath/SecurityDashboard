/**
 * @file led_status.c
 * @brief AtomS3 Lite WS2812 on GPIO 35 — faint green online, flash red offline.
 */
#include "led_status.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "led_status";

#ifndef CONFIG_LED_GPIO
#define CONFIG_LED_GPIO 35
#endif
#ifndef CONFIG_LED_GREEN_LEVEL
#define CONFIG_LED_GREEN_LEVEL 8
#endif

static led_strip_handle_t s_strip;
static volatile bool s_online;
static TaskHandle_t s_task;

static void led_task(void *arg)
{
    (void)arg;
    bool red_on = false;
    while (1) {
        if (s_online) {
            led_strip_set_pixel(s_strip, 0, 0, CONFIG_LED_GREEN_LEVEL, 0);
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            red_on = !red_on;
            if (red_on) {
                led_strip_set_pixel(s_strip, 0, 24, 0, 0);
            } else {
                led_strip_set_pixel(s_strip, 0, 0, 0, 0);
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

esp_err_t led_status_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_online = false;
    led_strip_set_pixel(s_strip, 0, 24, 0, 0);
    led_strip_refresh(s_strip);
    if (xTaskCreate(led_task, "led_status", 2048, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AtomS3 Lite LED on GPIO %d (faint green online, flash red offline)",
             CONFIG_LED_GPIO);
    return ESP_OK;
}

void led_status_set_online(bool online)
{
    s_online = online;
}
