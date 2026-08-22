idf_component_register(SRCS "ble_peripheral.c" "led_strip_encoder.c"
                        INCLUDE_DIRS "include"
                        PRIV_INCLUDE_DIRS "."
                        PRIV_REQUIRES bt esp_driver_rmt esp_driver_gpio nvs_flash ble_session_fsm usb_printer_host esp_app_format wifi_creds)
