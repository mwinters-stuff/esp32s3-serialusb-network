#include <memory>

#include "config.h"
#include "esp-mdns.h"
#include "led_indicator.h"
#include "w5500.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_eth.h>
#include <esp_eth_mac_spi.h>
#include <esp_eth_netif_glue.h>
#include <esp_eth_phy.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char *TAG = "W5500";

static esp_eth_handle_t s_eth_handle = nullptr;
static esp_netif_t *s_eth_netif = nullptr;
static bool s_got_ip = false;
static SemaphoreHandle_t s_ip_semaphore = nullptr;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
  auto led = static_cast<LedIndicator *>(arg);
  switch (event_id) {
  case ETHERNET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Ethernet link up");
    break;
  case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "Ethernet link down");
    s_got_ip = false;
    if (led)
      led->setState(LedState::WIFI_DISCONNECTED);
    break;
  default:
    break;
  }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  auto led = static_cast<LedIndicator *>(arg);
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
  s_got_ip = true;
  mdns_announce();
  if (led)
    led->setState(LedState::IDLE);
  if (s_ip_semaphore)
    xSemaphoreGive(s_ip_semaphore);
}

bool w5500_init(std::shared_ptr<LedIndicator> ledIndicator) {
#if !ENABLE_W5500_ETH
  ESP_LOGW(TAG, "W5500 Ethernet is disabled in config");
  return false;
#endif

  ESP_LOGI(TAG, "Initialising W5500 Ethernet...");

  // Install GPIO ISR service so the W5500 driver can register its interrupt
  // handler on the INT pin. Required when int_gpio_num != -1.
#if W5500_INT_PIN != -1
  // Use level 1 explicitly so the level-2 IRAM interrupt slot stays free for
  // USB host
  esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    // ESP_ERR_INVALID_STATE means already installed, which is fine
    ESP_LOGE(TAG, "gpio_install_isr_service failed: %s",
             esp_err_to_name(isr_err));
    return false;
  }
#endif

  if (ledIndicator) {
    ledIndicator->setState(LedState::WIFI_DISCONNECTED);
  }

  // Configure reset pin and pulse it before SPI init
  if (W5500_RST_PIN != -1) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << W5500_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level((gpio_num_t)W5500_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)W5500_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "W5500 reset complete");
  }

  // Initialize SPI bus
  spi_bus_config_t buscfg = {
      .mosi_io_num = W5500_MOSI_PIN,
      .miso_io_num = W5500_MISO_PIN,
      .sclk_io_num = W5500_SCK_PIN,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };

  esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
    return false;
  }

  // W5500 SPI device config — passed to ETH_W5500_DEFAULT_CONFIG by pointer
  // command_bits/address_bits must be 0; the W5500 driver builds its own frame
  // header
  spi_device_interface_config_t devcfg = {
      .command_bits = 0,
      .address_bits = 0,
      .mode = 0,
      .clock_speed_hz = 20 * 1000 * 1000,
      .spics_io_num = W5500_CS_PIN,
      .queue_size = 20,
  };

  // Build W5500 config using the proper macro (takes host id + devcfg pointer)
  eth_w5500_config_t w5500_config =
      ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
  w5500_config.int_gpio_num = W5500_INT_PIN;
#if W5500_INT_PIN == -1
  // Polling mode required when no interrupt pin — driver assertion: exactly one
  // of int_gpio_num >= 0 OR poll_period_ms > 0 must be true
  w5500_config.poll_period_ms = 10;
#endif

  // Build MAC config
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

  // Build PHY config
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.reset_gpio_num = -1; // Reset handled manually above

  // Create MAC and PHY instances
  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  if (mac == nullptr) {
    ESP_LOGE(TAG, "Failed to create W5500 MAC");
    return false;
  }

  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
  if (phy == nullptr) {
    ESP_LOGE(TAG, "Failed to create W5500 PHY");
    return false;
  }

  // Install Ethernet driver
  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  err = esp_eth_driver_install(&eth_config, &s_eth_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
    return false;
  }

  // Set MAC address from the ESP32's factory-burned base MAC
  // W5500 starts with 00:00:00:00:00:00 which DHCP servers reject
  uint8_t mac_addr[6];
  esp_read_mac(mac_addr, ESP_MAC_ETH);
  ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
  ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1],
           mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  // Create netif and attach Ethernet
  esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
  s_eth_netif = esp_netif_new(&netif_cfg);
  ESP_ERROR_CHECK(
      esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
  ESP_ERROR_CHECK(esp_netif_set_hostname(s_eth_netif, MDNS_HOSTNAME));

  // Register event handlers
  ESP_ERROR_CHECK(esp_event_handler_register(
      ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, ledIndicator.get()));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, ledIndicator.get()));

  // Start Ethernet
  s_ip_semaphore = xSemaphoreCreateBinary();
  ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
  ESP_LOGI(TAG, "Ethernet started, waiting for IP...");

  // Block until IP is acquired or 10-second timeout elapses
  bool got_ip = s_ip_semaphore &&
                xSemaphoreTake(s_ip_semaphore, pdMS_TO_TICKS(10000)) == pdTRUE;
  vSemaphoreDelete(s_ip_semaphore);
  s_ip_semaphore = nullptr;

  if (!got_ip) {
    ESP_LOGW(TAG, "Ethernet timeout waiting for IP");
    return false;
  }
  return true;
}

bool w5500_is_connected() { return s_got_ip && s_eth_netif != nullptr; }
