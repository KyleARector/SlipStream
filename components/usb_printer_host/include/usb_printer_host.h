#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the USB Host Library in host mode over the native USB-OTG
 * peripheral and starts background tasks that enumerate connected devices.
 *
 * Each connected device's descriptor (VID/PID, interface count) is logged
 * on connect; disconnect is logged cleanly with no crash/hang. This is
 * bring-up only -- no printer-specific I/O (bulk transfers, ESC/POS) yet.
 *
 * Returns once the background tasks are created; enumeration happens
 * asynchronously and continues for the lifetime of the app. */
esp_err_t usb_printer_host_start(void);

#ifdef __cplusplus
}
#endif
