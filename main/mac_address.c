/*
 * mac_address.c
 *
 * Created on: Dec 3, 2023
 * Author: alex technology
 */

#include <stdio.h>
#include <inttypes.h>
#include "esp_mac.h"

#include "mac_address.h"

BaseType_t mac_get_address(char *buffer, size_t buffer_len) {
    if (!(buffer && buffer_len > 0))
        return pdFAIL;

    mac_address_t mac;
    mac.as_long = 0;
    esp_efuse_mac_get_default(mac.as_bytes);
    snprintf(buffer, buffer_len, "%" PRIu64, mac.as_long);

    return pdPASS;
}
