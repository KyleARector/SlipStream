#include "usb_printer_host.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "escpos_formatter.h"
#include "print_job_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#define HOST_LIB_TASK_PRIORITY   2
#define ENUM_TASK_PRIORITY       3
#define HOST_LIB_TASK_STACK_SIZE 4096
/* Sized to fit an inline HTTPS image fetch (M25) -- esp_http_client + TLS
 * handshake needs meaningfully more than the 5KB this task ran with before
 * image jobs existed. The fetch runs on enum_task itself rather than a
 * dedicated task (contrast with M22's OTA download, which does get its own
 * task): it blocks enum_task for the fetch's duration, delaying USB event
 * processing and other queued jobs until it completes, which is an
 * accepted tradeoff for a low-traffic personal device with occasional
 * image jobs, not an oversight. Revisit with a dedicated fetch task if
 * that stops being true. */
#define ENUM_TASK_STACK_SIZE    (12 * 1024)
#define MAX_TRACKED_DEVICES      8
#define CLIENT_EVENT_POLL_MS     100
#define INCOMING_JOB_QUEUE_LEN   PRINT_JOB_QUEUE_CAPACITY

/* M8 queue demo: set to 1 to enqueue 3 hardcoded messages once the printer
 * is ready, proving the queue drains multiple jobs in order. Off by
 * default so plugging in the printer or rebooting during normal
 * development doesn't print (and cut!) every single time -- flip on when
 * you actually want to re-validate the queue behavior, then back off. */
#define ENABLE_QUEUE_DEMO 0

/* M23 text-size demo: set to 1 to send one hardcoded 3x3-sized test print
 * once the printer is ready, for visually confirming GS ! scaling on the
 * physical printer. Fires as its own direct transfer, bypassing the print
 * job FSM/queue entirely -- text size isn't part of the queued job data
 * model yet (that's later, job-type-extension work, not this milestone's
 * scope), so this exists purely to exercise escpos_format_sized() on real
 * hardware. Off by default, same reasoning as ENABLE_QUEUE_DEMO above. */
#define ENABLE_TEXT_SIZE_DEMO 0

/* M24 image demo: set to 1 to enqueue one hardcoded test bitmap once the
 * printer is ready. Unlike the text-size demo, this goes through the real
 * usb_printer_host_enqueue_image() -> queue -> FSM path (M24's acceptance
 * criterion is specifically that an image job prints via the *same* queue
 * path as text jobs, no bypass), proving the whole image plumbing works
 * end to end, not just the formatter in isolation. Off by default, same
 * reasoning as the other demo flags. */
#define ENABLE_IMAGE_DEMO 0

/* One-off empirical width test: set to 1 to print an 8-stripe, 576px-wide
 * bar (72px/stripe) once the printer is ready, to confirm the TM-H2000's
 * real usable print width in dots -- unconfirmed per the Phase 2 spec's
 * open item, and a datasheet figure (203dpi, 576 dots/2.83in) exists but
 * hadn't been checked against this specific physical unit. If 576 is
 * right, all 8 stripes print cleanly edge-to-edge with no clipping,
 * wraparound, or garbled trailing columns. Not meant to be a permanent
 * fixture -- remove once the real width is confirmed and recorded in the
 * spec. Off by default, same reasoning as the other demo flags. */
#define ENABLE_WIDTH_TEST_DEMO 0

/* Extra trailing feed lines purely so each print is visibly pushed past
 * the tear bar -- not part of escpos_format()'s tested output. */
#define PRINT_EXTRA_FEED_LINES 8

/* GS V 0 -- full cut. Not part of escpos_format()'s tested output;
 * appended here per job as a separate, deliberate addition. */
#define ESCPOS_CUT_FULL_LEN 3
static const uint8_t k_escpos_cut_full[ESCPOS_CUT_FULL_LEN] = {0x1D, 0x56, 0x00};

/* ESC @ -- initialize printer. Used as a recovery command: if a streamed
 * image aborts partway through (see stream_server_image()'s failure paths),
 * the printer has already been told via GS v 0's header how many raster
 * bytes to expect and won't leave raster mode until it receives that many --
 * otherwise it keeps swallowing everything printed afterward (including the
 * next job's own init sequence and text) as pixel data, producing garbled
 * output far longer than any single job's content. ESC @ forces the printer
 * back to a clean state regardless of what raster byte count it still
 * thinks it's owed. */
static const uint8_t k_escpos_init[ESCPOS_CMD_INIT_LEN] = {0x1B, 0x40};

static const char *TAG = "usb_printer_host";

/* Image job "reference resolution" (M24): the queue only ever carries a
 * reference string, never bitmap bytes (see print_job_fsm.h); this is
 * where a reference gets turned into actual pixels, right before
 * printing. For now this only recognizes one hardcoded demo reference and
 * serves a procedurally-generated checkerboard from RAM -- M25 replaces
 * this lookup with an HTTP fetch from the server, keyed by the same kind
 * of reference string, without needing to touch the queue/FSM again.
 *
 * Deliberately narrow (64px, 8 bytes/row): the TM-H2000's actual full
 * print width in dots is unconfirmed (see the Phase 2 spec's open item on
 * this) -- picking a small, arbitrary width here avoids baking in a wrong
 * assumption about the real printable width before that's verified. */
#define DEMO_IMAGE_REF          "demo-checkerboard"
#define DEMO_IMAGE_WIDTH_PX     64
#define DEMO_IMAGE_WIDTH_BYTES  (DEMO_IMAGE_WIDTH_PX / 8)
#define DEMO_IMAGE_HEIGHT_PX    32
#define DEMO_IMAGE_BLOCK_PX     8

static uint8_t s_demo_bitmap[DEMO_IMAGE_WIDTH_BYTES * DEMO_IMAGE_HEIGHT_PX];
static bool s_demo_bitmap_built;

static void build_demo_checkerboard(void)
{
    for (size_t row = 0; row < DEMO_IMAGE_HEIGHT_PX; row++) {
        size_t block_row = row / DEMO_IMAGE_BLOCK_PX;
        for (size_t col_byte = 0; col_byte < DEMO_IMAGE_WIDTH_BYTES; col_byte++) {
            /* Each byte is exactly one 8px-wide checkerboard block, since
             * DEMO_IMAGE_BLOCK_PX == 8 bits per byte. */
            bool black = ((block_row + col_byte) % 2) == 0;
            s_demo_bitmap[row * DEMO_IMAGE_WIDTH_BYTES + col_byte] = black ? 0xFF : 0x00;
        }
    }
    s_demo_bitmap_built = true;
}

/* Width-test pattern (see ENABLE_WIDTH_TEST_DEMO): 8 alternating vertical
 * stripes, each STRIPE_WIDTH_PX wide, spanning the full candidate width.
 * If the candidate width is actually printable edge-to-edge, all 8 stripes
 * appear with no clipping/wraparound. */
#define WIDTH_TEST_REF          "demo-width-test"
#define WIDTH_TEST_WIDTH_PX     576
#define WIDTH_TEST_WIDTH_BYTES  (WIDTH_TEST_WIDTH_PX / 8)
#define WIDTH_TEST_HEIGHT_PX    64
#define WIDTH_TEST_NUM_STRIPES  8
#define WIDTH_TEST_STRIPE_BYTES (WIDTH_TEST_WIDTH_BYTES / WIDTH_TEST_NUM_STRIPES)

static uint8_t s_width_test_bitmap[WIDTH_TEST_WIDTH_BYTES * WIDTH_TEST_HEIGHT_PX];
static bool s_width_test_bitmap_built;

static void build_width_test_stripes(void)
{
    for (size_t row = 0; row < WIDTH_TEST_HEIGHT_PX; row++) {
        for (size_t col_byte = 0; col_byte < WIDTH_TEST_WIDTH_BYTES; col_byte++) {
            size_t stripe_index = col_byte / WIDTH_TEST_STRIPE_BYTES;
            bool black = (stripe_index % 2) == 0;
            s_width_test_bitmap[row * WIDTH_TEST_WIDTH_BYTES + col_byte] = black ? 0xFF : 0x00;
        }
    }
    s_width_test_bitmap_built = true;
}

/* Real server-fetched image jobs (M25). api_client encodes the reference
 * as "{image_ref}:{width_dots}x{height_dots}" from the check-in response's
 * job fields (see usb_printer_host.h) -- carrying the dimensions in the
 * reference itself means the exact frame size is known before any bytes
 * are fetched.
 *
 * Bit polarity: GET /images/{image_ref} already returns bytes with
 * bit=1=print (server-side fixed to match GS v 0, not Pillow's own
 * opposite convention) -- no inversion needed here.
 *
 * A real server image (IMAGE_SOURCE_SERVER) is streamed to the printer in
 * bounded chunks (see stream_server_image() below) rather than held whole
 * in one buffer, so this is now just a sanity ceiling against a corrupt or
 * malicious width_dots*height_dots claim (e.g. a bad job that would have
 * the device try to download gigabytes) -- not a memory constraint. Sized
 * generously above slipstream-web's own MAX_IMAGE_OUTPUT_HEIGHT_DOTS
 * default (~144KB at full 576px width), so any real banner it can produce
 * fits comfortably under this. The two small in-RAM demo bitmaps don't go
 * through streaming at all and never come close to this size. */
#define IMAGE_FETCH_HTTP_TIMEOUT_MS 20000
#define IMAGE_FETCH_MAX_BITMAP_BYTES (2 * 1024 * 1024)
/* Chunk size for streaming a real server image to the printer -- small
 * enough to never be a meaningful memory concern (comfortably fits even
 * fragmented DMA-capable RAM), large enough to keep the number of
 * round-trip HTTP reads + USB transfers reasonable for a tall banner. */
#define IMAGE_STREAM_CHUNK_BYTES 4096

static const char *s_server_url;
static const char *s_api_key;

/* Which bitmap source a parsed reference resolved to -- see
 * parse_image_reference()/fill_image_bitmap() below. */
typedef enum {
    IMAGE_SOURCE_DEMO_CHECKERBOARD,
    IMAGE_SOURCE_DEMO_WIDTH_TEST,
    IMAGE_SOURCE_SERVER,
} image_source_t;

typedef struct {
    image_source_t source;
    char image_id[64]; /* only meaningful when source == IMAGE_SOURCE_SERVER */
    size_t width_bytes;
    size_t height_px;
} image_job_info_t;

/* Parses a reference and validates/reports its dimensions, but fetches no
 * bytes -- lets the caller compute the exact frame size and allocate the
 * USB transfer buffer *before* any bitmap data exists anywhere, so
 * fill_image_bitmap() below can write directly into that buffer instead
 * of a separate allocation. Returns false for an unrecognized reference,
 * a malformed "{ref}:{w}x{h}" encoding, or dimensions that would exceed
 * IMAGE_FETCH_MAX_BITMAP_BYTES. */
static bool parse_image_reference(const char *ref, size_t ref_len, image_job_info_t *out_info)
{
    if (ref_len == strlen(DEMO_IMAGE_REF) && memcmp(ref, DEMO_IMAGE_REF, ref_len) == 0) {
        out_info->source = IMAGE_SOURCE_DEMO_CHECKERBOARD;
        out_info->width_bytes = DEMO_IMAGE_WIDTH_BYTES;
        out_info->height_px = DEMO_IMAGE_HEIGHT_PX;
        return true;
    }

    if (ref_len == strlen(WIDTH_TEST_REF) && memcmp(ref, WIDTH_TEST_REF, ref_len) == 0) {
        out_info->source = IMAGE_SOURCE_DEMO_WIDTH_TEST;
        out_info->width_bytes = WIDTH_TEST_WIDTH_BYTES;
        out_info->height_px = WIDTH_TEST_HEIGHT_PX;
        return true;
    }

    /* Otherwise, expect "{image_ref}:{width_dots}x{height_dots}" -- see
     * usb_printer_host_enqueue_image()'s doc comment. image_ref is a UUID
     * (36 chars), but sized generously here rather than hardcoded to that
     * exact length. ref is NUL-terminated by print_job_queue_push()/the
     * incoming-job message copy, so treating it as a plain C string here
     * is safe. */
    unsigned width_dots = 0;
    unsigned height_dots = 0;
    if (ref_len >= sizeof(out_info->image_id)) {
        return false;
    }
    if (sscanf(ref, "%63[^:]:%ux%u", out_info->image_id, &width_dots, &height_dots) != 3) {
        return false;
    }
    if (width_dots == 0 || height_dots == 0) {
        return false;
    }

    size_t width_bytes = (width_dots + 7) / 8;
    size_t bitmap_len = width_bytes * height_dots;
    if (bitmap_len > IMAGE_FETCH_MAX_BITMAP_BYTES) {
        ESP_LOGE(TAG, "Image job dimensions too large (%ux%u dots)", width_dots, height_dots);
        return false;
    }

    out_info->source = IMAGE_SOURCE_SERVER;
    out_info->width_bytes = width_bytes;
    out_info->height_px = height_dots;
    return true;
}

/* Fills dest (sized info->width_bytes * info->height_px, already validated
 * by parse_image_reference()) with one of the small in-RAM demo patterns --
 * a plain memcpy, since both are at most a few KB. A real server image
 * (IMAGE_SOURCE_SERVER) never goes through this: see stream_server_image()
 * below, which sends it straight to the printer in bounded chunks instead
 * of ever assembling it in one buffer. */
static bool fill_demo_bitmap(const image_job_info_t *info, uint8_t *dest)
{
    size_t len = info->width_bytes * info->height_px;

    switch (info->source) {
    case IMAGE_SOURCE_DEMO_CHECKERBOARD:
        if (!s_demo_bitmap_built) {
            build_demo_checkerboard();
        }
        memcpy(dest, s_demo_bitmap, len);
        return true;

    case IMAGE_SOURCE_DEMO_WIDTH_TEST:
        if (!s_width_test_bitmap_built) {
            build_width_test_stripes();
        }
        memcpy(dest, s_width_test_bitmap, len);
        return true;

    case IMAGE_SOURCE_SERVER:
        return false; /* not this function's job -- see stream_server_image() */
    }

    return false;
}

typedef enum {
    DEVICE_ACTION_OPEN            = (1 << 0),
    DEVICE_ACTION_GET_DEV_DESC    = (1 << 1),
    DEVICE_ACTION_GET_CONFIG_DESC = (1 << 2),
    DEVICE_ACTION_CLOSE           = (1 << 3),
} device_action_t;

typedef struct {
    bool in_use;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint8_t pending_actions;
} tracked_device_t;

/* Message handed off from any other task into the print job queue via a
 * FreeRTOS queue. Per the spec's Concurrency Model, nothing outside
 * enum_task ever touches print_job_queue_t/print_job_fsm_t directly --
 * everyone else goes through usb_printer_host_enqueue_print() /
 * usb_printer_host_enqueue_image(). */
typedef struct {
    print_job_type_t type;
    char payload[PRINT_JOB_TEXT_MAX_LEN];
    size_t payload_len;
    bool cut_after;
} incoming_job_msg_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    SemaphoreHandle_t lock; /* protects devices[] and has_unhandled */
    tracked_device_t devices[MAX_TRACKED_DEVICES];
    bool has_unhandled;

    /* The one printer this driver knows how to talk to right now. Owned
     * solely by enum_task, same as the print job FSM/queue below. */
    bool printer_ready;
    usb_device_handle_t printer_dev_hdl;
    uint8_t printer_bulk_out_ep;

    /* Owned solely by enum_task -- see the spec's Concurrency Model. */
    print_job_fsm_t print_fsm;
    print_job_queue_t print_queue;
} enum_driver_t;

static enum_driver_t s_driver;
static QueueHandle_t s_incoming_jobs;

/* Set up once in usb_printer_host_start(); signaled from
 * stream_chunk_transfer_done_cb() when a chunk submitted by
 * send_chunk_blocking() finishes. */
static SemaphoreHandle_t s_chunk_done_sem;
static volatile usb_transfer_status_t s_last_chunk_status;

static void stream_chunk_transfer_done_cb(usb_transfer_t *transfer)
{
    s_last_chunk_status = transfer->status;
    usb_host_transfer_free(transfer);
    xSemaphoreGive(s_chunk_done_sem);
}

/* Submits one chunk (up to IMAGE_STREAM_CHUNK_BYTES) as its own USB
 * transfer and blocks -- still on enum_task, which is fine, see
 * ENUM_TASK_STACK_SIZE's comment on the accepted inline-fetch tradeoff --
 * until it completes. Pumps usb_host_client_handle_events() itself while
 * waiting, since that's what actually dispatches the completion callback
 * that signals s_chunk_done_sem; enum_task's own outer loop would
 * otherwise be the one doing this, so nothing new is competing for it. */
static bool send_chunk_blocking(const uint8_t *data, size_t len)
{
    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(len, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate image stream chunk transfer: %s", esp_err_to_name(err));
        return false;
    }

    memcpy(transfer->data_buffer, data, len);
    transfer->num_bytes = (int)len;
    transfer->device_handle = s_driver.printer_dev_hdl;
    transfer->bEndpointAddress = s_driver.printer_bulk_out_ep;
    transfer->callback = stream_chunk_transfer_done_cb;

    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit image stream chunk transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        return false;
    }

    while (xSemaphoreTake(s_chunk_done_sem, 0) != pdTRUE) {
        usb_host_client_handle_events(s_driver.client_hdl, pdMS_TO_TICKS(50));
    }

    if (s_last_chunk_status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "Image stream chunk transfer failed, status=%d", (int)s_last_chunk_status);
        return false;
    }
    return true;
}

/* Streams a real server image to the printer: header, then bitmap data in
 * IMAGE_STREAM_CHUNK_BYTES-sized pieces read directly off the still-open
 * HTTP response, then the trailer -- never assembling the whole image in
 * one buffer, however tall it is. This is what actually removes the
 * height ceiling a single-buffer approach would otherwise impose (the
 * 2MB IMAGE_FETCH_MAX_BITMAP_BYTES cap is a sanity check, not a memory
 * limit -- see its comment). Each piece is sent via send_chunk_blocking(),
 * so this blocks enum_task for the whole transfer, same accepted tradeoff
 * as the rest of the image-job path. Gated on cut_after the same way as
 * the single-shot path below: when false, the trailer is just the raster
 * data's own LF -- no extra feed padding, no cut -- so this job's strip
 * stays physically joined to whatever prints right after it. */
static bool stream_server_image(const image_job_info_t *info, bool cut_after)
{
    size_t total_len = info->width_bytes * info->height_px;

    char url[256];
    snprintf(url, sizeof(url), "%s/images/%s", s_server_url, info->image_id);
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = IMAGE_FETCH_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client for image stream");
        return false;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Image stream request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Image stream returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t header_buf[ESCPOS_CMD_INIT_LEN + ESCPOS_CMD_RASTER_HEADER_LEN];
    size_t header_len = escpos_format_raster_header(info->width_bytes, info->height_px, header_buf, sizeof(header_buf));
    if (header_len == 0 || !send_chunk_blocking(header_buf, header_len)) {
        ESP_LOGE(TAG, "Failed to send image stream header");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t chunk_buf[IMAGE_STREAM_CHUNK_BYTES];
    size_t remaining = total_len;
    bool ok = true;
    while (remaining > 0 && ok) {
        size_t want = remaining < sizeof(chunk_buf) ? remaining : sizeof(chunk_buf);
        int read_len = esp_http_client_read(client, (char *)chunk_buf, want);
        if (read_len <= 0) {
            ESP_LOGW(TAG, "Image stream read failed (%d) with %u bytes remaining", read_len, (unsigned)remaining);
            ok = false;
            break;
        }
        ok = send_chunk_blocking(chunk_buf, (size_t)read_len);
        remaining -= (size_t)read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        /* The printer already received the GS v 0 header promising
         * total_len raster bytes but got fewer than that -- force it out
         * of raster mode now rather than leaving it to misinterpret
         * whatever prints next as leftover pixel data. Best-effort: if
         * this transfer also fails, there's nothing further to do. */
        ESP_LOGW(TAG, "Image stream aborted with %u bytes remaining, resetting printer to recover from raster mode",
                 (unsigned)remaining);
        send_chunk_blocking(k_escpos_init, ESCPOS_CMD_INIT_LEN);
        return false;
    }

    uint8_t trailer_buf[1 + PRINT_EXTRA_FEED_LINES + ESCPOS_CUT_FULL_LEN];
    size_t trailer_len = 0;
    trailer_buf[trailer_len++] = 0x0A;
    if (cut_after) {
        for (int i = 0; i < PRINT_EXTRA_FEED_LINES; i++) {
            trailer_buf[trailer_len++] = '\n';
        }
        memcpy(&trailer_buf[trailer_len], k_escpos_cut_full, ESCPOS_CUT_FULL_LEN);
        trailer_len += ESCPOS_CUT_FULL_LEN;
    }

    return send_chunk_blocking(trailer_buf, trailer_len);
}

#if ENABLE_TEXT_SIZE_DEMO
static void print_text_size_demo(void);
#endif

/* Written only by enum_task, once per loop iteration below; read from any
 * task via usb_printer_host_queue_is_idle(). A single bool read/write is
 * atomic on this architecture, so no lock is needed for this one derived
 * flag even though print_queue/print_fsm themselves stay single-owner. */
static volatile bool s_queue_idle = true;

/* --- device action handlers: run only from enum_task, never from inside
 * the client event callback (USB Host Library functions must not be
 * called re-entrantly from that callback's context). --- */

static void action_open(tracked_device_t *dev)
{
    ESP_LOGI(TAG, "Opening device at address %d", dev->dev_addr);
    esp_err_t err = usb_host_device_open(s_driver.client_hdl, dev->dev_addr, &dev->dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open device at address %d: %s", dev->dev_addr, esp_err_to_name(err));
        return;
    }
    dev->pending_actions |= DEVICE_ACTION_GET_DEV_DESC;
}

static void action_get_dev_desc(tracked_device_t *dev)
{
    const usb_device_desc_t *desc;
    esp_err_t err = usb_host_get_device_descriptor(dev->dev_hdl, &desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get device descriptor: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Device connected: VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    usb_print_device_descriptor(desc);
    dev->pending_actions |= DEVICE_ACTION_GET_CONFIG_DESC;
}

/* Finds the first bulk OUT endpoint on interface 0, alt setting 0. Devices
 * with no bulk OUT endpoint (e.g. a hub sitting in the path) just aren't
 * treated as the printer. */
static bool find_bulk_out_endpoint(const usb_config_desc_t *config_desc, uint8_t *out_ep_addr)
{
    int offset = 0;
    const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(config_desc, 0, 0, &offset);
    if (intf_desc == NULL) {
        return false;
    }

    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        int ep_offset = 0;
        const usb_ep_desc_t *ep_desc =
            usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &ep_offset);
        if (ep_desc == NULL) {
            continue;
        }
        if (USB_EP_DESC_GET_XFERTYPE(ep_desc) == USB_TRANSFER_TYPE_BULK && USB_EP_DESC_GET_EP_DIR(ep_desc) == 0) {
            *out_ep_addr = ep_desc->bEndpointAddress;
            return true;
        }
    }
    return false;
}

static void action_get_config_desc(tracked_device_t *dev)
{
    const usb_config_desc_t *desc;
    esp_err_t err = usb_host_get_active_config_descriptor(dev->dev_hdl, &desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get config descriptor: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Device interface count: %d", desc->bNumInterfaces);
    usb_print_config_descriptor(desc, NULL);

    uint8_t ep_addr;
    if (!find_bulk_out_endpoint(desc, &ep_addr)) {
        ESP_LOGI(TAG, "No bulk OUT endpoint on device %d, not treating it as the printer", dev->dev_addr);
        return;
    }

    err = usb_host_interface_claim(s_driver.client_hdl, dev->dev_hdl, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to claim interface: %s", esp_err_to_name(err));
        return;
    }

    s_driver.printer_dev_hdl = dev->dev_hdl;
    s_driver.printer_bulk_out_ep = ep_addr;
    s_driver.printer_ready = true;
    ESP_LOGI(TAG, "Printer ready (bulk OUT endpoint 0x%02X)", ep_addr);

#if ENABLE_QUEUE_DEMO
    usb_printer_host_enqueue_print("Job 1 of 3", 10, true);
    usb_printer_host_enqueue_print("Job 2 of 3", 10, true);
    usb_printer_host_enqueue_print("Job 3 of 3", 10, true);
#endif

#if ENABLE_TEXT_SIZE_DEMO
    print_text_size_demo();
#endif

#if ENABLE_IMAGE_DEMO
    usb_printer_host_enqueue_image(DEMO_IMAGE_REF, strlen(DEMO_IMAGE_REF), true);
#endif

#if ENABLE_WIDTH_TEST_DEMO
    usb_printer_host_enqueue_image(WIDTH_TEST_REF, strlen(WIDTH_TEST_REF), true);
#endif
}

static void action_close(tracked_device_t *dev)
{
    ESP_LOGI(TAG, "Device disconnected (was address %d)", dev->dev_addr);
    if (dev->dev_hdl != NULL) {
        if (s_driver.printer_ready && s_driver.printer_dev_hdl == dev->dev_hdl) {
            s_driver.printer_ready = false;
            s_driver.printer_dev_hdl = NULL;
        }

        /* Release before close, matching IDF's own async host examples.
         * ESP_ERR_NOT_FOUND just means this device never had interface 0
         * claimed (e.g. it had no bulk OUT endpoint) -- not a real failure. */
        esp_err_t err = usb_host_interface_release(s_driver.client_hdl, dev->dev_hdl, 0);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to release interface: %s", esp_err_to_name(err));
        }

        err = usb_host_device_close(s_driver.client_hdl, dev->dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close device: %s", esp_err_to_name(err));
        }
    }
    memset(dev, 0, sizeof(*dev));
}

static void handle_device_actions(tracked_device_t *dev)
{
    uint8_t actions = dev->pending_actions;
    dev->pending_actions = 0;

    while (actions) {
        if (actions & DEVICE_ACTION_OPEN) {
            action_open(dev);
        }
        if (actions & DEVICE_ACTION_GET_DEV_DESC) {
            action_get_dev_desc(dev);
        }
        if (actions & DEVICE_ACTION_GET_CONFIG_DESC) {
            action_get_config_desc(dev);
        }
        if (actions & DEVICE_ACTION_CLOSE) {
            action_close(dev);
            break; /* slot was cleared, nothing else to process */
        }
        actions = dev->pending_actions;
        dev->pending_actions = 0;
    }
}

/* --- client event callback: runs in the USB Host Library's context.
 * Must not block or call USB Host Library functions directly, so it only
 * records what happened; the actual work happens in enum_task's loop. --- */

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    xSemaphoreTake(s_driver.lock, portMAX_DELAY);

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        tracked_device_t *slot = NULL;
        for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
            if (!s_driver.devices[i].in_use) {
                slot = &s_driver.devices[i];
                break;
            }
        }
        if (slot == NULL) {
            ESP_LOGW(TAG, "No free tracking slot for new device at address %d", event_msg->new_dev.address);
            break;
        }
        slot->in_use = true;
        slot->dev_addr = event_msg->new_dev.address;
        slot->dev_hdl = NULL;
        slot->pending_actions = DEVICE_ACTION_OPEN;
        s_driver.has_unhandled = true;
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
            if (s_driver.devices[i].in_use && s_driver.devices[i].dev_hdl == event_msg->dev_gone.dev_hdl) {
                s_driver.devices[i].pending_actions = DEVICE_ACTION_CLOSE;
                s_driver.has_unhandled = true;
            }
        }
        break;
    default:
        ESP_LOGW(TAG, "Unhandled client event: %d", event_msg->event);
        break;
    }

    xSemaphoreGive(s_driver.lock);
}

/* --- print job processing: all of this runs only on enum_task, which is
 * the single owning task for print_fsm/print_queue per the spec's
 * Concurrency Model. --- */

static void print_job_transfer_done_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Print job sent (%d bytes)", transfer->actual_num_bytes);
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_SENT);
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_PRINTED);
    } else {
        ESP_LOGE(TAG, "Print transfer failed, status=%d", transfer->status);
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
    }
    /* Deliberately does not release the interface here -- see action_close()
     * for why (num_urb_inflight isn't cleared until just after this callback
     * returns, so interface_release() can silently fail if called inline). */
    usb_host_transfer_free(transfer);
}

static void start_next_print_job(void)
{
    print_job_t job;
    if (!print_job_queue_pop(&s_driver.print_queue, &job)) {
        return;
    }

    print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_START);
    ESP_LOGI(TAG, "Starting print job: type=%d cut_after=%d payload_len=%u payload=\"%.*s\"", (int)job.type,
             (int)job.cut_after, (unsigned)job.payload_len, (int)job.payload_len, job.payload);

    image_job_info_t image_info;
    if (job.type == PRINT_JOB_TYPE_IMAGE) {
        if (!parse_image_reference(job.payload, job.payload_len, &image_info)) {
            ESP_LOGE(TAG, "Unable to resolve image reference: %.*s", (int)job.payload_len, job.payload);
            print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
            return;
        }

        if (image_info.source == IMAGE_SOURCE_SERVER) {
            /* Streamed in bounded chunks -- see stream_server_image()'s
             * comment for why this never assembles the whole image in one
             * buffer, however tall it is. By the time this returns, the
             * header, every data chunk, and the trailer have all already
             * been submitted and confirmed sent, so FORMATTED/SENT/PRINTED
             * fire together rather than at separate points in time -- an
             * honest simplification, not a shortcut: unlike the single-shot
             * path below, there's no meaningful "formatted but not yet
             * sent" moment to distinguish here. */
            bool ok = stream_server_image(&image_info, job.cut_after);
            if (ok) {
                print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_FORMATTED);
                print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_SENT);
                print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_PRINTED);
                ESP_LOGI(TAG, "Image job streamed and printed");
            } else {
                ESP_LOGE(TAG, "Failed to stream image job");
                print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
            }
            return;
        }
    }

    /* Single-shot path: text jobs and the two small in-RAM demo bitmaps.
     * The frame is built directly into the transfer's own heap-allocated
     * data_buffer, never staged in a separate buffer first. */
    size_t frame_capacity;
    if (job.type == PRINT_JOB_TYPE_TEXT) {
        frame_capacity = ESCPOS_FRAME_OVERHEAD_LEN + job.payload_len + PRINT_EXTRA_FEED_LINES + ESCPOS_CUT_FULL_LEN;
    } else {
        frame_capacity = ESCPOS_RASTER_FRAME_OVERHEAD_LEN + (image_info.width_bytes * image_info.height_px) +
                          PRINT_EXTRA_FEED_LINES + ESCPOS_CUT_FULL_LEN;
    }

    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(frame_capacity, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate print transfer: %s", esp_err_to_name(err));
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
        return;
    }

    size_t frame_len;
    if (job.type == PRINT_JOB_TYPE_TEXT) {
        frame_len = escpos_format(job.payload, job.payload_len, transfer->data_buffer, frame_capacity);
    } else {
        size_t header_len =
            escpos_format_raster_header(image_info.width_bytes, image_info.height_px, transfer->data_buffer,
                                         frame_capacity);
        if (header_len == 0 || !fill_demo_bitmap(&image_info, transfer->data_buffer + header_len)) {
            ESP_LOGE(TAG, "Failed to build image frame for queued job");
            usb_host_transfer_free(transfer);
            print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
            return;
        }
        frame_len = header_len + (image_info.width_bytes * image_info.height_px);
        transfer->data_buffer[frame_len++] = 0x0A; /* matches escpos_format_raster()'s own trailing LF */
    }

    if (frame_len == 0) {
        ESP_LOGE(TAG, "escpos formatter failed to produce output for queued job");
        usb_host_transfer_free(transfer);
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
        return;
    }
    print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_FORMATTED);

    if (job.cut_after) {
        for (int i = 0; i < PRINT_EXTRA_FEED_LINES; i++) {
            transfer->data_buffer[frame_len++] = '\n';
        }
        memcpy(&transfer->data_buffer[frame_len], k_escpos_cut_full, ESCPOS_CUT_FULL_LEN);
        frame_len += ESCPOS_CUT_FULL_LEN;
    }

    transfer->num_bytes = (int)frame_len;
    transfer->device_handle = s_driver.printer_dev_hdl;
    transfer->bEndpointAddress = s_driver.printer_bulk_out_ep;
    transfer->callback = print_job_transfer_done_cb;

    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit print transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_ERROR);
    }
}

#if ENABLE_TEXT_SIZE_DEMO
static void text_size_demo_transfer_done_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Text size demo print sent (%d bytes)", transfer->actual_num_bytes);
    } else {
        ESP_LOGE(TAG, "Text size demo transfer failed, status=%d", transfer->status);
    }
    usb_host_transfer_free(transfer);
}

/* Bypasses the print job FSM/queue entirely -- see ENABLE_TEXT_SIZE_DEMO's
 * comment above for why. */
static void print_text_size_demo(void)
{
    static const char k_demo_text[] = "3x3 SIZE TEST";
    uint8_t frame[ESCPOS_SIZED_FRAME_OVERHEAD_LEN + sizeof(k_demo_text) - 1 + PRINT_EXTRA_FEED_LINES +
                  ESCPOS_CUT_FULL_LEN];
    size_t frame_len = escpos_format_sized(k_demo_text, sizeof(k_demo_text) - 1, 3, 3, frame, sizeof(frame));
    if (frame_len == 0) {
        ESP_LOGE(TAG, "escpos_format_sized() failed to produce output for text size demo");
        return;
    }

    for (int i = 0; i < PRINT_EXTRA_FEED_LINES; i++) {
        frame[frame_len++] = '\n';
    }
    memcpy(&frame[frame_len], k_escpos_cut_full, ESCPOS_CUT_FULL_LEN);
    frame_len += ESCPOS_CUT_FULL_LEN;

    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(frame_len, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate text size demo transfer: %s", esp_err_to_name(err));
        return;
    }

    memcpy(transfer->data_buffer, frame, frame_len);
    transfer->num_bytes = (int)frame_len;
    transfer->device_handle = s_driver.printer_dev_hdl;
    transfer->bEndpointAddress = s_driver.printer_bulk_out_ep;
    transfer->callback = text_size_demo_transfer_done_cb;

    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit text size demo transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
    }
}
#endif

/* Drains anything handed off via usb_printer_host_enqueue_print() /
 * usb_printer_host_enqueue_image() into the pure-logic queue. Only
 * enum_task ever calls print_job_queue_push(). */
static void drain_incoming_jobs(void)
{
    incoming_job_msg_t msg;
    while (xQueueReceive(s_incoming_jobs, &msg, 0) == pdTRUE) {
        if (!print_job_queue_push(&s_driver.print_queue, msg.type, msg.payload, msg.payload_len, msg.cut_after)) {
            ESP_LOGW(TAG, "Print job queue full, dropping job: %.*s", (int)msg.payload_len, msg.payload);
        }
    }
}

/* Advances the print job FSM: observes a finished/errored job, resets it,
 * then starts the next queued job once the printer is ready and idle. */
static void service_print_queue(void)
{
    print_job_state_t state = print_job_fsm_get_state(&s_driver.print_fsm);

    if (state == PRINT_JOB_STATE_COMPLETE || state == PRINT_JOB_STATE_ERROR) {
        ESP_LOGI(TAG, "Print job cycle %s, resetting", state == PRINT_JOB_STATE_COMPLETE ? "complete" : "errored");
        print_job_fsm_handle_event(&s_driver.print_fsm, PRINT_JOB_EVENT_RESET);
        state = PRINT_JOB_STATE_IDLE;
    }

    if (state == PRINT_JOB_STATE_IDLE && s_driver.printer_ready && !print_job_queue_is_empty(&s_driver.print_queue)) {
        start_next_print_job();
    }

    /* Re-fetch state rather than reuse the local above -- start_next_print_job()
     * may have just transitioned the FSM out of IDLE, and this flag must never
     * report idle for even one extra loop iteration while a job is in flight. */
    s_queue_idle = (uxQueueMessagesWaiting(s_incoming_jobs) == 0) &&
                   print_job_queue_is_empty(&s_driver.print_queue) &&
                   print_job_fsm_get_state(&s_driver.print_fsm) == PRINT_JOB_STATE_IDLE;
}

bool usb_printer_host_queue_is_idle(void)
{
    return s_queue_idle;
}

esp_err_t usb_printer_host_enqueue_print(const char *text, size_t text_len, bool cut_after)
{
    if (text == NULL || text_len >= PRINT_JOB_TEXT_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    incoming_job_msg_t msg;
    msg.type = PRINT_JOB_TYPE_TEXT;
    memcpy(msg.payload, text, text_len);
    msg.payload_len = text_len;
    msg.cut_after = cut_after;

    if (xQueueSend(s_incoming_jobs, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t usb_printer_host_enqueue_image(const char *image_ref, size_t image_ref_len, bool cut_after)
{
    if (image_ref == NULL || image_ref_len >= PRINT_JOB_TEXT_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    incoming_job_msg_t msg;
    msg.type = PRINT_JOB_TYPE_IMAGE;
    memcpy(msg.payload, image_ref, image_ref_len);
    msg.payload_len = image_ref_len;
    msg.cut_after = cut_after;

    if (xQueueSend(s_incoming_jobs, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void enum_task(void *arg)
{
    (void)arg;

    s_driver.lock = xSemaphoreCreateMutex();
    if (s_driver.lock == NULL) {
        ESP_LOGE(TAG, "Failed to create device tracking mutex");
        vTaskSuspend(NULL);
        return;
    }

    print_job_fsm_init(&s_driver.print_fsm);
    print_job_queue_init(&s_driver.print_queue);

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &s_driver.client_hdl));
    ESP_LOGI(TAG, "USB client registered, waiting for devices");

    while (1) {
        drain_incoming_jobs();
        service_print_queue();

        bool unhandled;
        xSemaphoreTake(s_driver.lock, portMAX_DELAY);
        unhandled = s_driver.has_unhandled;
        xSemaphoreGive(s_driver.lock);

        if (unhandled) {
            xSemaphoreTake(s_driver.lock, portMAX_DELAY);
            for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
                if (s_driver.devices[i].in_use && s_driver.devices[i].pending_actions) {
                    handle_device_actions(&s_driver.devices[i]);
                }
            }
            s_driver.has_unhandled = false;
            xSemaphoreGive(s_driver.lock);
        } else {
            /* Bounded, not portMAX_DELAY: this loop also has to come back
             * around regularly to drain incoming print jobs and service the
             * print queue even when no USB client event is pending. */
            usb_host_client_handle_events(s_driver.client_hdl, pdMS_TO_TICKS(CLIENT_EVENT_POLL_MS));
        }
    }
}

static void host_lib_task(void *arg)
{
    TaskHandle_t notify_task = (TaskHandle_t)arg;

    ESP_LOGI(TAG, "Installing USB Host Library");
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host Library installed");

    xTaskNotifyGive(notify_task);

    while (1) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB Host Library reports no clients; unexpected during normal operation");
        }
    }
}

esp_err_t usb_printer_host_start(const char *server_url, const char *api_key)
{
    s_server_url = server_url;
    s_api_key = api_key;

    memset(&s_driver, 0, sizeof(s_driver));

    s_incoming_jobs = xQueueCreate(INCOMING_JOB_QUEUE_LEN, sizeof(incoming_job_msg_t));
    if (s_incoming_jobs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_chunk_done_sem = xSemaphoreCreateBinary();
    if (s_chunk_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t host_lib_task_hdl = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(host_lib_task, "usb_host_lib", HOST_LIB_TASK_STACK_SIZE,
                                                  xTaskGetCurrentTaskHandle(), HOST_LIB_TASK_PRIORITY,
                                                  &host_lib_task_hdl, 0);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    /* Wait for the library install to complete before registering a client. */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for USB Host Library install");
        return ESP_ERR_TIMEOUT;
    }

    TaskHandle_t enum_task_hdl = NULL;
    created = xTaskCreatePinnedToCore(enum_task, "usb_printer_enum", ENUM_TASK_STACK_SIZE, NULL, ENUM_TASK_PRIORITY,
                                       &enum_task_hdl, 0);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
