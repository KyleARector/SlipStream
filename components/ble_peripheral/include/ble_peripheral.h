#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the pairing button + onboard RGB LED + BLE GATT peripheral.
 *
 * Holding the pairing button for 3s from IDLE starts BLE advertising (LED
 * blinks); connecting and writing text to the GATT characteristic logs it
 * verbatim over console and enqueues it as a print job (via
 * usb_printer_host_enqueue_print()), so it comes out on paper. Advertising
 * times out after 30s with no connection, returning to IDLE (LED off); a
 * deliberate disconnect instead resumes advertising immediately. Driven
 * entirely by components/ble_session_fsm -- see that component for the
 * state machine itself.
 *
 * A second, read-only characteristic on the same service reports the
 * running firmware version (same string as the boot banner) -- readable at
 * any time, no pairing/hold required.
 *
 * Two more write-only characteristics accept WiFi SSID and password
 * separately (no delimiter parsing) and persist them to NVS immediately.
 * No bonding/encryption is required to write them for v1 -- an accepted
 * risk given the pairing button's physical-proximity requirement. */
esp_err_t ble_peripheral_start(void);

#ifdef __cplusplus
}
#endif
