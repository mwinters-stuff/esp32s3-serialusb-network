idf_component_register(
    SRCS "usb-handler.cpp" "http-server.cpp" "main.cpp" "esp-mdns.cpp" "wifi.cpp" "littlefs.cpp" "http-server.cpp" "usb-handler.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_https_ota app_update 
    PRIV_REQUIRES usb
    )
littlefs_create_partition_image(littlefs ../littlefs FLASH_IN_PROJECT)