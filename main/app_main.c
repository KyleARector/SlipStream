#include "esp_idf_version.h"
#include "esp_log.h"

#include "usb_printer_host.h"

static const char *TAG = "slipstream";

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " SlipStream firmware");
    ESP_LOGI(TAG, " Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, " ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "================================================");

    ESP_ERROR_CHECK(usb_printer_host_start());
}
