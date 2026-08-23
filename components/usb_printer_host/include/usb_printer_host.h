#pragma once

#include <stdbool.h>
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
 * server_url and api_key (from secrets.h) are used to fetch real image job
 * bytes from GET {server_url}/images/{image_ref} (M25) when a queued image
 * job is dequeued -- must stay valid for the life of the program; pass the
 * string-literal constants directly, same convention as
 * api_client_start(). Only main.c ever touches secrets.h.
 *
 * Returns once the background tasks are created; enumeration happens
 * asynchronously and continues for the lifetime of the app. */
esp_err_t usb_printer_host_start(const char *server_url, const char *api_key);

/* Enqueues text as a print job. Safe to call from any task or context --
 * hands off via a FreeRTOS queue to the single task that owns the print
 * job FSM and its queue (see the spec's Concurrency Model); never touches
 * those pure-logic structures directly.
 *
 * cut_after controls whether the printer feeds+cuts after this job before
 * starting the next queued one -- pass false to let this job's strip stay
 * physically joined to whatever prints right after it (e.g. an image
 * immediately followed by its "From: @user" attribution as one continuous
 * receipt instead of two separate strips). Callers that don't care about
 * this should pass true, matching the printer's previous unconditional
 * per-job cut behavior.
 *
 * Returns ESP_ERR_INVALID_ARG if text is NULL or text_len is too long for
 * a single job, ESP_ERR_TIMEOUT if the handoff queue is full. */
esp_err_t usb_printer_host_enqueue_print(const char *text, size_t text_len, bool cut_after);

/* Enqueues an image job carrying only a reference/handle string, not image
 * bytes (M24) -- the actual bitmap is resolved when the job is dequeued for
 * printing, since image payloads don't fit a queue slot sized for text.
 * Same safety/handoff guarantees as usb_printer_host_enqueue_print().
 *
 * image_ref must be either one of the built-in demo reference strings, or
 * "{image_ref}:{width_dots}x{height_dots}" (M25) -- the exact encoding
 * api_client builds from a check-in response's image job fields, so the
 * dequeue-time fetch knows how many bytes to read without a second
 * round-trip just to ask the size.
 *
 * cut_after has the same meaning as usb_printer_host_enqueue_print()'s --
 * see its doc comment.
 *
 * Returns ESP_ERR_INVALID_ARG if image_ref is NULL or image_ref_len is too
 * long for a single job, ESP_ERR_TIMEOUT if the handoff queue is full. */
esp_err_t usb_printer_host_enqueue_image(const char *image_ref, size_t image_ref_len, bool cut_after);

/* True when there's no incoming/queued/in-flight print job anywhere in the
 * pipeline (the handoff queue, the pure-logic print_job_queue, and the FSM
 * itself are all idle). A single volatile flag, computed and written only
 * by enum_task (the sole owner of the print job FSM/queue per the spec's
 * Concurrency Model) and safe for any other task to read -- e.g. ota_update
 * (M22) uses this to defer flashing a downloaded image until there's no
 * job that would be lost by a reboot. */
bool usb_printer_host_queue_is_idle(void);

#ifdef __cplusplus
}
#endif
