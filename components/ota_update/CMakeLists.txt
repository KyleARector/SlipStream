idf_component_register(SRCS "ota_update.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_http_client mbedtls app_update esp_app_format usb_printer_host)
