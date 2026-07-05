
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

#include "esp-mdns.h"
#include "w5500.h"
#include "littlefs.h"
#include "http-server.h"
#include "usb-handler.h"
#include "led_indicator.h"
#include "wifi.h"


static const char *TAG = "MAIN";

static void init_network_stack()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
}

extern "C" void app_main(void)
{
    auto ledIndicator = std::make_shared<LedIndicator>();
    ledIndicator->init();

    mount_littlefs();

    init_network_stack();

    bool eth_connected = w5500_init(ledIndicator);
    if (!eth_connected)
    {
        ESP_LOGI(TAG, "Ethernet not available, falling back to WiFi");
        wifi_init_sta(ledIndicator);
        ESP_LOGI(TAG, "Network path active: WiFi fallback");
    }
    else
    {
        ESP_LOGI(TAG, "Ethernet connected successfully");
        ESP_LOGI(TAG, "Network path active: W5500 Ethernet");
    }

    initialise_mdns();
    auto usbHandler = std::make_shared<UsbHandler>(ledIndicator);
    auto httpServer = std::make_shared<HttpServer>(usbHandler, ledIndicator);
    httpServer->start();
    usbHandler->usb_loop();

    ESP_LOGE(TAG, "usb_loop exited unexpectedly; keeping app_main alive");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
    
}
