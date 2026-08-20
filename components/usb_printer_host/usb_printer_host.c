#include "usb_printer_host.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
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
} device_action_t;

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
}

static void action_close(tracked_device_t *dev)
{
    ESP_LOGI(TAG, "Device disconnected (was address %d)", dev->dev_addr);
    if (dev->dev_hdl != NULL) {
        esp_err_t err = usb_host_device_close(s_driver.client_hdl, dev->dev_hdl);
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
