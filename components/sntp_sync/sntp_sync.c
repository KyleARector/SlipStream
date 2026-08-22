#include "sntp_sync.h"

#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "sntp_sync";
static bool s_initialized;
static volatile bool s_synced;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "System time synced: %s UTC", buf);
}

bool sntp_sync_has_synced(void)
{
    return s_synced;
}

esp_err_t sntp_sync_start(void)
{
    if (s_initialized) {
        /* Already set up (e.g. from an earlier WiFi connect) -- just
         * restart, per esp_netif_sntp_start()'s documented behavior. */
        return esp_netif_sntp_start();
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = time_sync_notification_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}
