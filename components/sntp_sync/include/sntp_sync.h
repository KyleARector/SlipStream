#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts SNTP time sync. Call once WiFi has an IP (wifi_station does this
 * from its IP_EVENT_STA_GOT_IP handler) -- SNTP needs network connectivity
 * to reach its time server, and re-syncs automatically on every subsequent
 * got-IP event (e.g. after a reconnect).
 *
 * Non-blocking: returns once the sync request is issued, not once it
 * completes. Logs the synced date/time via a completion callback. Any
 * future HTTPS/TLS client (M21) must wait for a completed sync before its
 * first request -- some TLS stacks reject certificates as invalid while the
 * clock still reads its power-on default. */
esp_err_t sntp_sync_start(void);

#ifdef __cplusplus
}
#endif
