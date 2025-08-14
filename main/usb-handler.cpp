#include <memory>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"

#include "esp_http_server.h"

#include "usb-handler.h"
static const char *TAG = "VCP";

using namespace esp_usb;
// Buffer for received data

/**
 * @brief Data received callback
 *
 * Just pass received data to stdout
 *
 * @param[in] data     Pointer to received data
 * @param[in] data_len Length of received data in bytes
 * @param[in] arg      Argument we passed to the device open function
 * @return
 *   true:  We have processed the received data
 *   false: We expect more data
 */
bool UsbHandler::handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
  // ESP_LOGI(TAG, "handle_rx called, data_len=%d", (int)data_len);
  printf("%.*s", (int)data_len, data);

  // Buffer the data for web interface
  xSemaphoreTake(serial_buffer_mutex, portMAX_DELAY);
  size_t copy_len = data_len;
  if (serial_buffer_len + copy_len > sizeof(serial_buffer))
  {
    // If buffer full, reset
    serial_buffer_len = 0;
    copy_len = data_len < sizeof(serial_buffer) ? data_len : sizeof(serial_buffer);
  }
  memcpy(serial_buffer + serial_buffer_len, data, copy_len);
  serial_buffer_len += copy_len;
  xSemaphoreGive(serial_buffer_mutex);

  return true;
}

/**
 * @brief Device event callback
 *
 * Apart from handling device disconnection it doesn't do anything useful
 *
 * @param[in] event    Device event type and data
 * @param[in] user_ctx Argument we passed to the device open function
 */
void UsbHandler::handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
  switch (event->type)
  {
  case CDC_ACM_HOST_ERROR:
    ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %d", event->data.error);
    break;
  case CDC_ACM_HOST_DEVICE_DISCONNECTED:
    ESP_LOGI(TAG, "Device suddenly disconnected");
    xSemaphoreGive(device_disconnected_sem);
    break;
  case CDC_ACM_HOST_SERIAL_STATE:
    ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
    break;
  case CDC_ACM_HOST_NETWORK_CONNECTION:
  default:
    break;
  }
}

/**
 * @brief USB Host library handling task
 *
 * @param arg Unused
 */
void UsbHandler::usb_lib_task(void *arg)
{
  while (1)
  {
    // Start handling system events
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
    {
      ESP_ERROR_CHECK(usb_host_device_free_all());
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
    {
      ESP_LOGI(TAG, "USB: All devices freed");
      // Continue handling USB events to allow device reconnection
    }
  }
}

UsbHandler::UsbHandler()
{
  device_disconnected_sem = xSemaphoreCreateBinary();
  assert(device_disconnected_sem);

  serial_buffer_mutex = xSemaphoreCreateMutex();
  assert(serial_buffer_mutex);
  serial_buffer_len = 0;
}

void UsbHandler::usb_loop()
{

  // Install USB Host driver. Should only be called once in entire application
  ESP_LOGI(TAG, "Installing USB Host");
  usb_host_config_t host_config = {};
  host_config.skip_phy_setup = false;
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&host_config));

  // Create a task that will handle USB library events
  BaseType_t task_created = xTaskCreate(
      [](void *param)
      {
        static_cast<UsbHandler *>(param)->usb_lib_task(param);
      },
      "usb_lib", 4096, this, 10, NULL);
  assert(task_created == pdTRUE);

  ESP_LOGI(TAG, "Installing CDC-ACM driver");
  ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

  // Register VCP drivers to VCP service
  VCP::register_driver<FT23x>();
  VCP::register_driver<CP210x>();
  VCP::register_driver<CH34x>();

  // Do everything else in a loop, so we can demonstrate USB device reconnections
  while (true)
  {
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 64,
        .in_buffer_size = 64,
        .event_cb = [](const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
        { static_cast<UsbHandler *>(user_ctx)->handle_event(event, user_ctx); },

        .data_cb = [](const uint8_t *data, size_t data_len, void *user_arg)
        { return static_cast<UsbHandler *>(user_arg)->handle_rx(data, data_len, user_arg); },
        .user_arg = this,
    };

    ESP_LOGI(TAG, "Opening any VCP device...");
    vcp = std::unique_ptr<CdcAcmDevice>(VCP::open(&dev_config));

    if (vcp == nullptr)
    {
      ESP_LOGI(TAG, "Failed to open VCP device, retrying...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    ESP_LOGI(TAG, "Setting up line coding");
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = BAUDRATE,
        .bCharFormat = STOP_BITS,
        .bParityType = PARITY,
        .bDataBits = DATA_BITS,
    };
    ESP_ERROR_CHECK(vcp->line_coding_set(&line_coding));
    ESP_ERROR_CHECK(vcp->set_control_line_state(true, true));

    ESP_LOGI(TAG, "VCP device connected. Waiting for disconnection...");
    // Stay connected and process data until device is disconnected
    xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);

    ESP_LOGI(TAG, "VCP device disconnected. Cleaning up...");
    vcp.reset();
    // Optionally clear serial buffer here if desired
    serial_buffer_len = 0;
  }
}

esp_err_t UsbHandler::tx_blocking(uint8_t *data, size_t len)
{
  if (vcp)
  {
    return vcp->tx_blocking(data, len);
  }
  return ESP_FAIL;
}

esp_err_t UsbHandler::serial_get_handler(httpd_req_t *req)
{
  xSemaphoreTake(serial_buffer_mutex, portMAX_DELAY);
  auto error = httpd_resp_send(req, serial_buffer, serial_buffer_len);
  xSemaphoreGive(serial_buffer_mutex);
  return error;
}