if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR
        "main/secrets.h is missing. Copy main/secrets.h.example to "
        "main/secrets.h and fill in API_SERVER_URL and API_KEY before "
        "building. secrets.h is gitignored -- it never gets committed.")
endif()

idf_component_register(SRCS "app_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES usb_printer_host ble_peripheral esp_app_format wifi_station api_client)
