#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compares latest_version (as reported by a check-in response) to the
 * running firmware version (esp_app_get_description()->version). If they
 * match, does nothing. If they differ:
 *
 *   - If the print job queue isn't idle (usb_printer_host_queue_is_idle()),
 *     defers -- logs and returns without downloading anything. The queue
 *     lives in RAM, so an OTA reboot with jobs still in flight would lose
 *     them; the caller (api_client) naturally retries this check on its
 *     next successful poll.
 *   - Otherwise, downloads the signed image from
 *     GET {server_url}/firmware/{latest_version} (Bearer auth, same as
 *     check-in), streaming it into the inactive OTA partition (never the
 *     currently-running one) while holding back the trailing
 *     [DER signature][2-byte big-endian length] footer in a small RAM
 *     buffer. Verifies the image's own structural validity via
 *     esp_ota_end(), then its ECDSA-P256/SHA-256 signature against the
 *     embedded trust key (ota_trust_key.h). A corrupted or unsigned/
 *     mis-signed image is rejected at this point -- the boot partition is
 *     never switched, so the device just keeps running its current
 *     firmware, unaffected. A valid image is set as the next boot
 *     partition and the device reboots into it immediately. */
void ota_update_check_and_apply(const char *server_url, const char *api_key, const char *latest_version);

/* Call after a successful post-OTA check-in. If the running partition is
 * still in ESP_OTA_IMG_PENDING_VERIFY (i.e. this is the first successful
 * poll since an OTA reboot), marks it permanently bootable so the
 * bootloader won't auto-revert on a future reset -- the rollback-safety
 * mechanism required by the spec. A no-op if the partition is already
 * valid, or rollback support isn't compiled in. */
void ota_update_confirm_valid_if_pending(void);

#ifdef __cplusplus
}
#endif
