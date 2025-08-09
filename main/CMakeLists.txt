idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_https_ota app_update 
    PRIV_REQUIRES usb
    )
littlefs_create_partition_image(littlefs ../littlefs FLASH_IN_PROJECT)