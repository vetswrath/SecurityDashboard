/**
 * Host unit tests for Apple iBeacon advertisement parsing.
 * Build: make -C firmware/atom-ble/tests/host
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ble_ibeacon.h"

static int g_fail;

static void expect_true(const char *name, bool cond)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        g_fail++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    /* Classic iBeacon AD: flags + manufacturer (Apple 0x004C, type 0x02). */
    uint8_t adv[] = {
        0x02, 0x01, 0x06,
        0x1A, 0xFF,
        0x4C, 0x00,
        0x02, 0x15,
        0x42, 0x6C, 0x75, 0x65, 0x43, 0x68, 0x61, 0x72,
        0x6D, 0x42, 0x65, 0x61, 0x63, 0x6F, 0x6E, 0x73,
        0x0E, 0xFE,   /* major 3838 */
        0x13, 0x55,   /* minor 4949 */
        0xC5
    };

    ble_ibeacon_t ib;
    expect_true("parse georgia", ble_ibeacon_parse(adv, sizeof(adv), &ib));
    expect_true("uuid bluecharm", ble_ibeacon_uuid_is_bluecharm(ib.uuid));
    expect_true("major 3838", ib.major == 3838);
    expect_true("minor 4949", ib.minor == 4949);

    /* Second dog planned minor 4950 */
    uint8_t adv2[] = {
        0x02, 0x01, 0x06,
        0x1A, 0xFF,
        0x4C, 0x00,
        0x02, 0x15,
        0x42, 0x6C, 0x75, 0x65, 0x43, 0x68, 0x61, 0x72,
        0x6D, 0x42, 0x65, 0x61, 0x63, 0x6F, 0x6E, 0x73,
        0x0E, 0xFE,
        0x13, 0x56,   /* minor 4950 */
        0xC5
    };
    expect_true("parse second dog", ble_ibeacon_parse(adv2, sizeof(adv2), &ib));
    expect_true("minor 4950", ib.minor == 4950);

    uint8_t not_apple[] = {
        0x05, 0xFF, 0x59, 0x00, 0x01, 0x02, 0x03
    };
    expect_true("reject other company", !ble_ibeacon_parse(not_apple, sizeof(not_apple), &ib));

    uint8_t other_uuid[] = {
        0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        0x00, 0x01, 0x00, 0x02, 0xC5
    };
    expect_true("parse other uuid", ble_ibeacon_parse(other_uuid, sizeof(other_uuid), &ib));
    expect_true("not bluecharm", !ble_ibeacon_uuid_is_bluecharm(ib.uuid));

    uint8_t empty[] = {0};
    expect_true("reject empty", !ble_ibeacon_parse(empty, 0, &ib));

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
