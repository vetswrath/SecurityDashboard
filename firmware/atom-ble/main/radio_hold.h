/**
 * @file radio_hold.h
 * @brief Pause CSI UDP + BLE scan so LAN OTA can finish on 2.4 GHz.
 */
#ifndef RADIO_HOLD_H
#define RADIO_HOLD_H

#include <stdbool.h>

void radio_hold_pause(const char *reason);
void radio_hold_resume(void);
bool radio_hold_is_paused(void);

#endif /* RADIO_HOLD_H */
