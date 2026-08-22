#include "wifi_station.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sntp_sync.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_station";

#define RETRY_BACKOFF_INITIAL_MS 1000
#define RETRY_BACKOFF_MAX_MS     60000

static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_backoff_ms;

/* All touched only from the default esp_event loop task (the WIFI_EVENT/
 * IP_EVENT handler below, and the retry timer callback, which esp_timer
 * also dispatches on a single dedicated task) -- no cross-task locking
 * needed, same reasoning as the FSM/queue single-owner-task rule, just with
 * esp_event's/esp_timer's own task standing in for a task we'd otherwise
 * have to write ourselves. */
static void schedule_retry(void)
{
    s_retry_backoff_ms = (s_retry_backoff_ms == 0) ? RETRY_BACKOFF_INITIAL_MS : s_retry_backoff_ms * 2;
    if (s_retry_backoff_ms > RETRY_BACKOFF_MAX_MS) {
        s_retry_backoff_ms = RETRY_BACKOFF_MAX_MS;
    }
    ESP_LOGI(TAG, "Retrying WiFi connection in %" PRIu32 " ms", s_retry_backoff_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_backoff_ms * 1000);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, connecting...");
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        schedule_retry();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_backoff_ms = 0; /* reset so the next disconnect starts from the initial delay again */

        /* SNTP needs network connectivity, so this is the earliest point
         * to start it; it also re-syncs correctly on a later reconnect. */
        esp_err_t sntp_err = sntp_sync_start();
        if (sntp_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start SNTP sync: %s", esp_err_to_name(sntp_err));
        }
    }
}

esp_err_t wifi_sta_start(void)
{
    char ssid[WIFI_CREDS_SSID_MAX_LEN + 1];
    char password[WIFI_CREDS_PASSWORD_MAX_LEN + 1];

    if (wifi_creds_get(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGI(TAG, "No stored WiFi credentials -- staying BLE-only");
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    /* Confirmed WPA2-PSK for the target network -- see the Phase 2 spec's
     * open item, resolved before scoping this milestone. */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station starting for SSID \"%s\"", ssid);
    return ESP_OK;
}
