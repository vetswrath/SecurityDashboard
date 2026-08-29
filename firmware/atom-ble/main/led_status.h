#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t led_status_init(void);
void led_status_set_online(bool online);

#endif /* LED_STATUS_H */
