idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_https_ota
    PRIV_REQUIRES usb
    )
