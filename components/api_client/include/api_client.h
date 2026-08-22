#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a background task that periodically checks in with the
 * SlipStream backend (`GET {server_url}/checkin`, `Authorization: Bearer
 * {api_key}` -- see slipstream-web's app/main.py, the authoritative
 * contract). server_url and api_key must stay valid for the life of the
 * program -- pass the string-literal constants from secrets.h directly;
 * this deliberately doesn't include secrets.h itself, so the "only main.c
 * ever touches secrets.h" boundary from M17 stays intact.
 *
 * Each poll: enqueues any returned text jobs into the print queue (via
 * usb_printer_host_enqueue_print(), the same entry point BLE uses -- see
 * the original spec's integration note) and logs the server's reported
 * latest_firmware_version (observed only for now; M22 will act on it).
 *
 * Waits for WiFi connectivity (wifi_station.h) and a completed SNTP sync
 * (sntp_sync.h, once) before its first request -- some TLS stacks reject
 * certs while the clock still reads its power-on default. Polls every
 * API_CLIENT_POLL_INTERVAL_MS regardless of individual request
 * success/failure; a failed poll (network blip, server down, bad
 * response) is logged as a warning and retried next interval, never
 * fatal. */
esp_err_t api_client_start(const char *server_url, const char *api_key);

#ifdef __cplusplus
}
#endif
