#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"

#include "api_client.h"
#include "ble_peripheral.h"
#include "secrets.h"
#include "usb_printer_host.h"
#include "wifi_station.h"

static const char *TAG = "slipstream";

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " SlipStream firmware");
    ESP_LOGI(TAG, " Version: %s", app_desc->version);
    ESP_LOGI(TAG, " Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, " ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "================================================");

    ESP_ERROR_CHECK(usb_printer_host_start());
    ESP_ERROR_CHECK(ble_peripheral_start());
    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(api_client_start(API_SERVER_URL, API_KEY));
}
