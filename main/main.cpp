#include "http-server.h"
#include "led_indicator.h"
#include "littlefs.h"
#include "usb-handler.h"
#include "w5500.h"
#include "web-logger.h"
#include "wifi.h"
#include <esp-mdns.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

static const char *TAG = "app_main";

extern "C" void app_main(void) {
  auto ledIndicator = std::make_shared<LedIndicator>();
  ledIndicator->init();

  mount_littlefs();

  // Common network stack init required by both ethernet and WiFi paths
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Try ethernet first, fall back to WiFi if not available
  bool eth_connected = w5500_init(ledIndicator);

  if (!eth_connected) {
    ESP_LOGI(TAG, "Ethernet not available, falling back to WiFi");
    wifi_init_sta(ledIndicator);
  } else {
    ESP_LOGI(TAG, "Ethernet connected successfully");
  }

  initialise_mdns();
  auto usbHandler = std::make_shared<UsbHandler>(ledIndicator);
  auto httpServer = std::make_shared<HttpServer>(usbHandler, ledIndicator);
  httpServer->start();

  // Initialize web logging after HTTP server is created
  WebLogger::init(httpServer);

  usbHandler->usb_loop();
}
