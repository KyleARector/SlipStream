#pragma once

#include <stdbool.h>

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

/* True once at least one sync has completed (never clears back to false
 * afterward, even across a later resync -- callers only need to know the
 * clock became trustworthy at some point, not track every resync). Poll
 * this with a short delay loop rather than blocking on it directly --
 * esp_netif_sntp's own wait primitive uses a semaphore that's only
 * re-signaled on each actual resync (which may be hours away), so a
 * caller that consumes it once would hang on a second wait. */
bool sntp_sync_has_synced(void);

#ifdef __cplusplus
}
#endif
