#include "ble_peripheral.h"

#include <string.h>

#include "ble_session_fsm.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"
#include "nvs_flash.h"

/* BLE */
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* Implemented in the bundled NimBLE host (store/config/src/ble_store_config.c)
 * but not declared in its own public header -- every official esp-idf NimBLE
 * example forward-declares it locally the same way. */
void ble_store_config_init(void);

/* Pairing button: GPIO4, active-low with internal pull-up (one leg to the
 * pin, other leg to GND) -- matches how this board's own BOOT button is
 * wired, per its schematic. */
#define PAIRING_BUTTON_GPIO   GPIO_NUM_4
#define BUTTON_POLL_MS        20
#define BUTTON_DEBOUNCE_MS    50
#define BUTTON_HOLD_MS        3000
#define ADVERTISING_TIMEOUT_MS 30000

/* Onboard WS2812B RGB LED: GPIO48, confirmed via this board's schematic
 * (net RGB_CTRL -> GPIO48, through a 0-ohm resistor, to the LED's DI pin).
 * The pinout reference image disagreed (labeled GPIO47) but the schematic
 * netlist is authoritative. */
#define RGB_LED_GPIO              48
#define RGB_LED_RMT_RESOLUTION_HZ 10000000
#define RGB_LED_BLINK_PERIOD_MS   500

#define BLE_DEVICE_NAME        "SlipStream"
#define BLE_WRITE_CHAR_MAX_LEN 256

static const char *TAG = "ble_peripheral";

/* --- onboard RGB LED (WS2812B over RMT) --- */

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_encoder;

static esp_err_t led_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RGB_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RGB_LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        return err;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = RGB_LED_RMT_RESOLUTION_HZ,
    };
    err = rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder);
    if (err != ESP_OK) {
        return err;
    }

    return rmt_enable(s_led_chan);
}

/* WS2812B wire order is G,R,B (per the RMT encoder's own bit-order
 * comment). The vendored example's demo array-fill code used G,B,R
 * instead -- harmless for its rainbow-chase demo where exact channel
 * identity doesn't matter, but it swaps red/blue for us. Confirmed on
 * real hardware: blue rendered as red until this was corrected. */
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t pixel[3] = {g, r, b};
    rmt_transmit_config_t tx_config = {.loop_count = 0};
    rmt_transmit(s_led_chan, s_led_encoder, pixel, sizeof(pixel), &tx_config);
}

/* --- BLE GATT service: one write-only characteristic --- */

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x51, 0x49, 0x50, 0x53, 0x74, 0x72, 0x65, 0x61,
                      0x6d, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
static const ble_uuid128_t s_chr_uuid =
    BLE_UUID128_INIT(0x51, 0x49, 0x50, 0x53, 0x74, 0x72, 0x65, 0x61,
                      0x6d, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
static uint16_t s_chr_val_handle;

/* Events handed from the NimBLE host task (GAP/GATT callbacks) to
 * ble_peripheral_task, which is the single task that owns ble_session_fsm
 * -- same discipline the spec's Concurrency Model requires for the print
 * job FSM/queue, extended here to the BLE session FSM. */
typedef enum {
    BLE_PERIPH_EVT_CONNECTED,
    BLE_PERIPH_EVT_DISCONNECTED,
    BLE_PERIPH_EVT_WRITE_RECEIVED,
} ble_periph_evt_t;

static QueueHandle_t s_ble_events;
static uint8_t s_own_addr_type;

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_chr_val_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[BLE_WRITE_CHAR_MAX_LEN];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(TAG, "BLE write received (%u bytes): %.*s", len, (int)len, buf);

    ble_periph_evt_t evt = BLE_PERIPH_EVT_WRITE_RECEIVED;
    xQueueSend(s_ble_events, &evt, 0);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_chr_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &s_chr_val_handle,
                },
                {0},
            },
    },
    {0},
};

static void on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)ctxt;
    (void)arg;
}

/* --- GAP: advertising + connection events --- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    ble_periph_evt_t evt;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE central connect attempt; status=%d", event->connect.status);
        if (event->connect.status == 0) {
            evt = BLE_PERIPH_EVT_CONNECTED;
            xQueueSend(s_ble_events, &evt, 0);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE central disconnected; reason=%d", event->disconnect.reason);
        evt = BLE_PERIPH_EVT_DISCONNECTED;
        xQueueSend(s_ble_events, &evt, 0);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertisement fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure BLE address: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to determine BLE address type: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "NimBLE host synced; hold the pairing button for %ds to advertise", BUTTON_HOLD_MS / 1000);
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- pairing button (debounce + 3s hold) + LED + FSM: all owned by this
 * one task, matching the print job FSM's single-owner discipline. --- */

static void ble_peripheral_task(void *arg)
{
    (void)arg;

    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(PAIRING_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);

    bool last_raw_pressed = false;
    bool stable_pressed = false;
    TickType_t last_change_tick = xTaskGetTickCount();
    TickType_t press_start_tick = 0;
    bool fired_this_press = false;

    TickType_t advertising_start_tick = 0;
    bool led_blink_on = false;
    TickType_t last_blink_toggle_tick = xTaskGetTickCount();

    led_set(0, 0, 0);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        /* Debounce: only accept the raw level once it's been steady for
         * BUTTON_DEBOUNCE_MS. Filters electrical noise/bounce. */
        bool raw_pressed = (gpio_get_level(PAIRING_BUTTON_GPIO) == 0);
        if (raw_pressed != last_raw_pressed) {
            last_raw_pressed = raw_pressed;
            last_change_tick = now;
        }
        if (stable_pressed != raw_pressed && (now - last_change_tick) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
            stable_pressed = raw_pressed;
            if (stable_pressed) {
                press_start_tick = now;
                fired_this_press = false;
            }
        }

        /* Hold: only treat a debounced press as intentional once it's been
         * held continuously for BUTTON_HOLD_MS. Guards against accidental
         * bumps/brief catches. ble_session_fsm_handle_event() itself
         * enforces "only actioned from IDLE" -- a press while ADVERTISING
         * or CONNECTED is a no-op here for free. */
        if (stable_pressed && !fired_this_press && (now - press_start_tick) >= pdMS_TO_TICKS(BUTTON_HOLD_MS)) {
            fired_this_press = true;
            if (ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS)) {
                ESP_LOGI(TAG, "Pairing button held %ds -- starting BLE advertising", BUTTON_HOLD_MS / 1000);
                start_advertising();
                advertising_start_tick = now;
            }
        }

        /* Drain BLE events from the NimBLE host task. RECEIVE_START then
         * RECEIVE_COMPLETE fire back to back for a single write -- BLE
         * writes are atomic at this transport level, there's no separate
         * "in-progress" phase to observe, same simplification as M8's
         * SENDING/PRINTING/COMPLETE collapse for print jobs. */
        ble_periph_evt_t evt;
        while (xQueueReceive(s_ble_events, &evt, 0) == pdTRUE) {
            switch (evt) {
            case BLE_PERIPH_EVT_CONNECTED:
                ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_PEER_CONNECTED);
                break;
            case BLE_PERIPH_EVT_DISCONNECTED:
                ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_TIMEOUT);
                /* A disconnect (as opposed to an advertising window
                 * elapsing with nobody ever connecting) resumes advertising
                 * right away, without requiring another physical button
                 * hold. This re-fires the same IDLE->ADVERTISING path a
                 * button hold uses; it's a policy decision made here in
                 * the hardware glue, not a new FSM transition. */
                if (ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS)) {
                    ESP_LOGI(TAG, "Disconnected -- resuming advertising");
                    start_advertising();
                    advertising_start_tick = now;
                }
                break;
            case BLE_PERIPH_EVT_WRITE_RECEIVED:
                ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_START);
                ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_COMPLETE);
                break;
            }
        }

        /* 30s advertising timeout -- read state fresh, after draining
         * events above, so a connection that just landed this same
         * iteration isn't mistaken for a still-advertising timeout. */
        ble_session_state_t state = ble_session_fsm_get_state(&fsm);
        if (state == BLE_SESSION_STATE_ADVERTISING &&
            (now - advertising_start_tick) >= pdMS_TO_TICKS(ADVERTISING_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "Advertising timed out after %ds with no connection", ADVERTISING_TIMEOUT_MS / 1000);
            ble_gap_adv_stop();
            ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_TIMEOUT);
            state = ble_session_fsm_get_state(&fsm);
        }

        switch (state) {
        case BLE_SESSION_STATE_IDLE:
            led_set(0, 0, 0);
            break;
        case BLE_SESSION_STATE_ADVERTISING:
            if ((now - last_blink_toggle_tick) >= pdMS_TO_TICKS(RGB_LED_BLINK_PERIOD_MS)) {
                led_blink_on = !led_blink_on;
                last_blink_toggle_tick = now;
                led_set(0, 0, led_blink_on ? 40 : 0);
            }
            break;
        case BLE_SESSION_STATE_CONNECTED:
        case BLE_SESSION_STATE_RECEIVING:
            led_set(0, 40, 0);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t ble_peripheral_start(void)
{
    s_ble_events = xQueueCreate(8, sizeof(ble_periph_evt_t));
    if (s_ble_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RGB LED: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = on_gatt_register;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT service config: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set BLE device name: %d", rc);
    }

    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);

    BaseType_t created = xTaskCreate(ble_peripheral_task, "ble_peripheral", 4096, NULL, 3, NULL);
    if (created != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
