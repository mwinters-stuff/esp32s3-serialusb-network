
#include <algorithm>
#include <memory>
#include <stdio.h>
#include <string.h>

#include "config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "nvs_flash.h"

#include "led_indicator.h"

void wifi_init_sta(std::shared_ptr<LedIndicator> ledIndicator)
{
    ledIndicator->setState(LedState::WIFI_DISCONNECTED);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, MDNS_HOSTNAME); // Set your desired hostname

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("wifi", "Wi-Fi started, connecting to SSID: %s", wifi_config.sta.ssid);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE("wifi", "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }

    // Wait for connection and print IP
    ESP_LOGI("wifi", "Waiting for connection...");
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, [](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
        auto led = static_cast<LedIndicator *>(arg);
        if (event_id == WIFI_EVENT_STA_CONNECTED)
        {
            ESP_LOGI("wifi", "Connected to AP");
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            ESP_LOGW("wifi", "Disconnected from AP");
            if (led) led->setState(LedState::WIFI_DISCONNECTED);
        } }, ledIndicator.get(), &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
        auto led = static_cast<LedIndicator *>(arg);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("wifi", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (led) led->setState(LedState::IDLE); }, ledIndicator.get(), &instance_got_ip));

    // Wait until got IP
    while (true)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
        {
            ESP_LOGI("wifi", "Connected with IP: " IPSTR, IP2STR(&ip_info.ip));
            break;
        }
    }
}
