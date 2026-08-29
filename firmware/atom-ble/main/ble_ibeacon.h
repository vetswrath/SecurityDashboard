/**
 * @file ble_ibeacon.h
 * @brief Host-testable Apple iBeacon advertisement parser.
 *
 * Looks for manufacturer data: company 0x004C, type 0x02, length 0x15.
 */
#ifndef BLE_IBEACON_H
#define BLE_IBEACON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BLE_IBEACON_COMPANY_ID  0x004C
#define BLE_IBEACON_TYPE        0x02
#define BLE_IBEACON_BODY_LEN    0x15
#define BLE_IBEACON_UUID_LEN    16

/** Default Blue Charm BC021 Pro proximity UUID (ASCII "BlueCharmBeacons"). */
#define BLE_BLUECHARM_UUID_BYTES \
    0x42, 0x6C, 0x75, 0x65, 0x43, 0x68, 0x61, 0x72, \
    0x6D, 0x42, 0x65, 0x61, 0x63, 0x6F, 0x6E, 0x73

typedef struct {
    uint8_t  uuid[BLE_IBEACON_UUID_LEN];
    uint16_t major;
    uint16_t minor;
    int8_t   tx_power;
} ble_ibeacon_t;

/**
 * Parse a BLE advertisement payload (AD structures) for an iBeacon.
 *
 * @param adv      Advertisement data (not including MAC).
 * @param adv_len  Length of adv.
 * @param out      Filled on success.
 * @return true if a well-formed iBeacon was found.
 */
bool ble_ibeacon_parse(const uint8_t *adv, size_t adv_len, ble_ibeacon_t *out);

/** Return true if uuid matches the 16-byte Blue Charm default. */
bool ble_ibeacon_uuid_is_bluecharm(const uint8_t uuid[BLE_IBEACON_UUID_LEN]);

#endif /* BLE_IBEACON_H */
