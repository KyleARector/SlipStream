#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the USB Host Library in host mode over the native USB-OTG
 * peripheral and starts background tasks that enumerate connected devices.
 *
 * Each connected device's descriptor (VID/PID, interface count) is logged
 * on connect; disconnect is logged cleanly with no crash/hang. Once a
 * device with a bulk OUT endpoint (the printer) is found, it drives the
 * print job FSM/queue from components/print_job_fsm -- see
 * usb_printer_host_enqueue_print().
 *
 * Returns once the background tasks are created; enumeration happens
 * asynchronously and continues for the lifetime of the app. */
esp_err_t usb_printer_host_start(void);

/* Enqueues text as a print job. Safe to call from any task or context --
 * hands off via a FreeRTOS queue to the single task that owns the print
 * job FSM and its queue (see the spec's Concurrency Model); never touches
 * those pure-logic structures directly.
 *
 * Returns ESP_ERR_INVALID_ARG if text is NULL or text_len is too long for
 * a single job, ESP_ERR_TIMEOUT if the handoff queue is full. */
esp_err_t usb_printer_host_enqueue_print(const char *text, size_t text_len);

#ifdef __cplusplus
}
#endif
