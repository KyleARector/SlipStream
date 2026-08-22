#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

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
 * physical presence, and a reboot is a small ask during initial setup.
 *
 * Also kicks off SNTP time sync (see sntp_sync.h) as soon as an IP address
 * is obtained, and again on every subsequent reconnect. */
esp_err_t wifi_sta_start(void);

/* Blocks the calling task until WiFi has an IP, or until timeout ticks
 * elapse (pass portMAX_DELAY to wait forever). Returns ESP_OK once
 * connected, ESP_ERR_TIMEOUT on timeout, or ESP_ERR_INVALID_STATE if
 * wifi_sta_start() never brought up the WiFi driver at all (no stored
 * credentials). Safe to call from any task -- e.g. api_client (M21) uses
 * this to defer its first poll until there's a network to poll over. */
esp_err_t wifi_sta_wait_connected(TickType_t timeout);

#ifdef __cplusplus
}
#endif
