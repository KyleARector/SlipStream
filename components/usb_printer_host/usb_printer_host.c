#include "usb_printer_host.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "escpos_formatter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#define HOST_LIB_TASK_PRIORITY   2
#define ENUM_TASK_PRIORITY       3
#define HOST_LIB_TASK_STACK_SIZE 4096
#define ENUM_TASK_STACK_SIZE     (5 * 1024)
#define MAX_TRACKED_DEVICES      8

static const char *TAG = "usb_printer_host";

typedef enum {
    DEVICE_ACTION_OPEN            = (1 << 0),
    DEVICE_ACTION_GET_DEV_DESC    = (1 << 1),
    DEVICE_ACTION_GET_CONFIG_DESC = (1 << 2),
    DEVICE_ACTION_CLOSE           = (1 << 3),
    DEVICE_ACTION_PRINT_TEST      = (1 << 4),
} device_action_t;

/* M7 scope: print one hardcoded string to prove the formatter + USB host
 * path work end to end. Extra trailing feed lines are appended purely so
 * the print is visibly pushed past the tear bar -- not part of
 * escpos_format()'s tested output (that stays exactly init + text + one
 * line feed, per M5). */
#define PRINT_TEST_TEXT             "Hello from SlipStream!"
#define PRINT_TEST_EXTRA_FEED_LINES 8

/* GS V 0 -- full cut. Cut-command support is unconfirmed against the
 * physical printer per the spec's open item -- sending this from the
 * ESP32 (not just the earlier Mac-side script) is itself part of
 * confirming it. Deliberately NOT part of escpos_format()'s tested
 * output; appended here as a separate, experimental addition. */
#define ESCPOS_CUT_FULL_LEN 3
static const uint8_t k_escpos_cut_full[ESCPOS_CUT_FULL_LEN] = {0x1D, 0x56, 0x00};

typedef struct {
    bool in_use;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint8_t pending_actions;
} tracked_device_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    SemaphoreHandle_t lock; /* protects devices[] and has_unhandled */
    tracked_device_t devices[MAX_TRACKED_DEVICES];
    bool has_unhandled;
} enum_driver_t;

static enum_driver_t s_driver;

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
    dev->pending_actions |= DEVICE_ACTION_PRINT_TEST;
}

/* Finds the first bulk OUT endpoint on interface 0, alt setting 0. Devices
 * with no bulk OUT endpoint (e.g. the hub itself, when one sits between us
 * and the printer) simply don't get a print attempt. */
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

/* Runs once the print transfer completes (or fails); never called from
 * inside the client event callback, only from usb_host_client_handle_events()
 * further down the enum_task loop -- see the submit-and-return note below.
 *
 * Deliberately does NOT release the claimed interface here: the driver
 * doesn't consider the endpoint's URB fully retired until just after this
 * callback returns, so releasing inline can silently fail with
 * ESP_ERR_INVALID_STATE (interface_release() checks num_urb_inflight).
 * The interface stays claimed until action_close() releases it right
 * before closing the device -- same release-then-close ordering IDF's own
 * async host tests use, just deferred to actual disconnect instead of
 * happening inline after each transfer. */
static void print_transfer_done_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Print job sent (%d bytes)", transfer->actual_num_bytes);
    } else {
        ESP_LOGE(TAG, "Print transfer failed, status=%d", transfer->status);
    }

    usb_host_transfer_free(transfer);
}

/* Formats and submits one hardcoded print job. Submits and returns
 * immediately -- must NOT block waiting for print_transfer_done_cb, since
 * that callback only fires from within usb_host_client_handle_events(),
 * which this same task calls later in its own loop. Blocking here would
 * deadlock the task against itself. */
static void action_print_test(tracked_device_t *dev)
{
    const usb_config_desc_t *config_desc;
    esp_err_t err = usb_host_get_active_config_descriptor(dev->dev_hdl, &config_desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get config descriptor for print test: %s", esp_err_to_name(err));
        return;
    }

    uint8_t ep_addr;
    if (!find_bulk_out_endpoint(config_desc, &ep_addr)) {
        ESP_LOGI(TAG, "No bulk OUT endpoint on device %d, skipping print test", dev->dev_addr);
        return;
    }

    err = usb_host_interface_claim(s_driver.client_hdl, dev->dev_hdl, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to claim interface for print test: %s", esp_err_to_name(err));
        return;
    }

    static const char k_text[] = PRINT_TEST_TEXT;
    uint8_t frame[ESCPOS_FRAME_OVERHEAD_LEN + sizeof(k_text) - 1 + PRINT_TEST_EXTRA_FEED_LINES + ESCPOS_CUT_FULL_LEN];

    size_t frame_len = escpos_format(k_text, sizeof(k_text) - 1, frame, sizeof(frame));
    if (frame_len == 0) {
        ESP_LOGE(TAG, "escpos_format() failed to produce output");
        usb_host_interface_release(s_driver.client_hdl, dev->dev_hdl, 0);
        return;
    }

    for (int i = 0; i < PRINT_TEST_EXTRA_FEED_LINES; i++) {
        frame[frame_len++] = '\n';
    }

    memcpy(&frame[frame_len], k_escpos_cut_full, ESCPOS_CUT_FULL_LEN);
    frame_len += ESCPOS_CUT_FULL_LEN;
    ESP_LOGI(TAG, "Print job includes a full cut command -- watch the printer");

    usb_transfer_t *transfer = NULL;
    err = usb_host_transfer_alloc(frame_len, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate print transfer: %s", esp_err_to_name(err));
        usb_host_interface_release(s_driver.client_hdl, dev->dev_hdl, 0);
        return;
    }

    memcpy(transfer->data_buffer, frame, frame_len);
    transfer->num_bytes = (int)frame_len;
    transfer->device_handle = dev->dev_hdl;
    transfer->bEndpointAddress = ep_addr;
    transfer->callback = print_transfer_done_cb;

    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit print transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        usb_host_interface_release(s_driver.client_hdl, dev->dev_hdl, 0);
    }
}

static void action_close(tracked_device_t *dev)
{
    ESP_LOGI(TAG, "Device disconnected (was address %d)", dev->dev_addr);
    if (dev->dev_hdl != NULL) {
        /* Release before close, matching IDF's own async host examples.
         * ESP_ERR_NOT_FOUND just means this device never had interface 0
         * claimed (e.g. it had no bulk OUT endpoint, so action_print_test()
         * skipped it) -- not a real failure. */
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
        if (actions & DEVICE_ACTION_PRINT_TEST) {
            action_print_test(dev);
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

static void enum_task(void *arg)
{
    (void)arg;

    s_driver.lock = xSemaphoreCreateMutex();
    if (s_driver.lock == NULL) {
        ESP_LOGE(TAG, "Failed to create device tracking mutex");
        vTaskSuspend(NULL);
        return;
    }

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
            usb_host_client_handle_events(s_driver.client_hdl, portMAX_DELAY);
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

esp_err_t usb_printer_host_start(void)
{
    memset(&s_driver, 0, sizeof(s_driver));

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
