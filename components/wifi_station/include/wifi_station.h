#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts WiFi station mode using SSID/password stored in NVS (written via
 * ble_peripheral's WiFi credential characteristics -- see wifi_creds.h).
 *
 * If no credentials are stored yet, logs that and returns immediately
 * without touching the WiFi driver at all -- the device stays fully
 * BLE-only, same as Phase 1. If credentials are present but the network is
 * unreachable (wrong password, out of range, AP down), retries indefinitely
 * with exponential backoff (capped) rather than giving up; this never
 * blocks the caller or any other task, so BLE stays fully responsive
 * throughout.
 *
 * Credentials written via BLE after this has already run (and found none)
 * are not picked up live -- a reboot is required to attempt a fresh
 * connection. Acceptable for v1: the BLE pairing button already requires
 * physical presence, and a reboot is a small ask during initial setup. */
esp_err_t wifi_sta_start(void);

#ifdef __cplusplus
}
#endif
