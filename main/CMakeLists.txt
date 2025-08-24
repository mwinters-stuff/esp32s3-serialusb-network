idf_component_register(
    SRCS "led_indicator.cpp" "usb-handler.cpp" "http-server.cpp" "main.cpp" "esp-mdns.cpp" "wifi.cpp" "littlefs.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_https_ota app_update led_strip
    PRIV_REQUIRES usb
    )
littlefs_create_partition_image(littlefs ../littlefs FLASH_IN_PROJECT)