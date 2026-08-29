#include "ble_ibeacon.h"

#include <string.h>

static const uint8_t k_bluecharm_uuid[BLE_IBEACON_UUID_LEN] = {
    BLE_BLUECHARM_UUID_BYTES
};

static bool parse_mfg(const uint8_t *data, size_t len, ble_ibeacon_t *out)
{
    /* company_id(2) + type(1) + body_len(1) + uuid(16) + major(2) + minor(2) + tx(1) */
    if (data == NULL || out == NULL || len < 25) {
        return false;
    }
    uint16_t company = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (company != BLE_IBEACON_COMPANY_ID) {
        return false;
    }
    if (data[2] != BLE_IBEACON_TYPE || data[3] != BLE_IBEACON_BODY_LEN) {
        return false;
    }
    memcpy(out->uuid, &data[4], BLE_IBEACON_UUID_LEN);
    out->major = ((uint16_t)data[20] << 8) | data[21];
    out->minor = ((uint16_t)data[22] << 8) | data[23];
    out->tx_power = (int8_t)data[24];
    return true;
}

bool ble_ibeacon_parse(const uint8_t *adv, size_t adv_len, ble_ibeacon_t *out)
{
    if (adv == NULL || out == NULL) {
        return false;
    }
    size_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0) {
            break;
        }
        if (i + 1 + field_len > adv_len) {
            break;
        }
        uint8_t ad_type = adv[i + 1];
        if (ad_type == 0xFF) {
            if (parse_mfg(&adv[i + 2], (size_t)field_len - 1, out)) {
                return true;
            }
        }
        i += (size_t)field_len + 1;
    }
    return false;
}

bool ble_ibeacon_uuid_is_bluecharm(const uint8_t uuid[BLE_IBEACON_UUID_LEN])
{
    if (uuid == NULL) {
        return false;
    }
    return memcmp(uuid, k_bluecharm_uuid, BLE_IBEACON_UUID_LEN) == 0;
}
