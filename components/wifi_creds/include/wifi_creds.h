#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared NVS-backed storage for BLE-delivered WiFi credentials. Kept as its
 * own tiny component -- rather than duplicated between ble_peripheral
 * (writer, M18) and wifi_station (reader, M19) -- so the NVS namespace/key
 * names and length limits live in exactly one place. 63 bytes is WPA2-PSK's
 * max passphrase length; 32 is 802.11's max SSID length. */

#define WIFI_CREDS_NVS_NAMESPACE    "wifi_creds"
#define WIFI_CREDS_KEY_SSID         "ssid"
#define WIFI_CREDS_KEY_PASSWORD     "password"
#define WIFI_CREDS_SSID_MAX_LEN     32
#define WIFI_CREDS_PASSWORD_MAX_LEN 63

/* Persists one credential value (WIFI_CREDS_KEY_SSID or
 * WIFI_CREDS_KEY_PASSWORD) to NVS, committing before returning. */
esp_err_t wifi_creds_set(const char *key, const char *value);

/* Reads both stored values into caller-provided buffers (sized at least
 * WIFI_CREDS_*_MAX_LEN + 1). Returns an NVS error (e.g.
 * ESP_ERR_NVS_NOT_FOUND) if either value isn't stored yet -- callers should
 * treat any non-ESP_OK return as "no usable credentials". */
esp_err_t wifi_creds_get(char *ssid_out, size_t ssid_cap, char *password_out, size_t password_cap);

#ifdef __cplusplus
}
#endif
