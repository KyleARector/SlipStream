idf_component_register(SRCS "api_client.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_http_client mbedtls json wifi_station sntp_sync usb_printer_host ota_update)
