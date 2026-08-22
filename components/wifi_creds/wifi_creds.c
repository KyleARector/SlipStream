#include "wifi_creds.h"

#include "nvs.h"

esp_err_t wifi_creds_set(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_CREDS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_creds_get(char *ssid_out, size_t ssid_cap, char *password_out, size_t password_cap)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_CREDS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = ssid_cap;
    err = nvs_get_str(nvs, WIFI_CREDS_KEY_SSID, ssid_out, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    size_t password_len = password_cap;
    err = nvs_get_str(nvs, WIFI_CREDS_KEY_PASSWORD, password_out, &password_len);
    nvs_close(nvs);
    return err;
}
