#include <memory>
#include <string>
#include <stdlib.h>
#include <cctype>

#include "config.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <usb/cdc_acm_host.h>
#include <usb/usb_host.h>
#include <usb/vcp.hpp>
#include <usb/vcp_ch34x.hpp>
// #include "usb/vcp_cp210x.hpp"
// #include "usb/vcp_ftdi.hpp"

#include <esp_http_server.h>

#include "usb-handler.h"
static const char *TAG = "VCP";

using namespace esp_usb;

namespace
{
bool s_usb_host_installed = false;
bool s_usb_lib_task_started = false;
bool s_cdc_acm_installed = false;
constexpr size_t RX_LINE_MAX_LEN = 512;

std::string sanitize_for_log(const std::string &line)
{
  std::string sanitized;
  sanitized.reserve(line.size());

  for (unsigned char ch : line)
  {
    if (std::isprint(ch) || ch == '\t')
    {
      sanitized.push_back(static_cast<char>(ch));
    }
    else
    {
      sanitized.push_back('.');
    }
  }

  return sanitized;
}
}

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
  if (data_len == 0 || !rx_callback || rx_queue == NULL)
  {
    return true;
  }

  uint8_t *payload = static_cast<uint8_t *>(malloc(data_len));
  if (payload == NULL)
  {
    ESP_LOGW(TAG, "Dropping RX packet: out of memory (%d bytes)", (int)data_len);
    return true;
  }

  memcpy(payload, data, data_len);

  RxMessage message = {
      .data = payload,
      .len = data_len,
  };

  if (xQueueSend(rx_queue, &message, 0) != pdTRUE)
  {
    ESP_LOGW(TAG, "Dropping RX packet: dispatch queue full");
    free(payload);
  }

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

void UsbHandler::rx_dispatch_task()
{
  RxMessage message;

  while (true)
  {
    if (xQueueReceive(rx_queue, &message, portMAX_DELAY) != pdTRUE)
    {
      continue;
    }

    for (size_t i = 0; i < message.len; ++i)
    {
      const char ch = static_cast<char>(message.data[i]);

      if (ch == '\n')
      {
        flush_rx_line(true);
        continue;
      }

      if (ch == '\r')
      {
        continue;
      }

      rx_line_buffer.push_back(ch);
      if (rx_line_buffer.size() >= RX_LINE_MAX_LEN)
      {
        ESP_LOGW(TAG, "RX line exceeded %u bytes, flushing partial line", (unsigned)RX_LINE_MAX_LEN);
        flush_rx_line(false);
      }
    }

    free(message.data);
  }
}

void UsbHandler::flush_rx_line(bool with_newline)
{
  if (rx_line_buffer.empty() && !with_newline)
  {
    return;
  }

  std::string outbound = rx_line_buffer;
  if (with_newline)
  {
    outbound.push_back('\n');
  }

  if (rx_callback && !outbound.empty())
  {
    rx_callback(reinterpret_cast<const uint8_t *>(outbound.data()), outbound.size());
  }

  ESP_LOGI(TAG, "RX line: %s", sanitize_for_log(rx_line_buffer).c_str());
  rx_line_buffer.clear();
}

UsbHandler::UsbHandler(std::shared_ptr<LedIndicator> led) : rx_queue(NULL), rx_task_handle(NULL), ledIndicator(led)
{
  device_disconnected_sem = xSemaphoreCreateBinary();
  assert(device_disconnected_sem);

  rx_queue = xQueueCreate(32, sizeof(RxMessage));
  assert(rx_queue);

  BaseType_t task_created = xTaskCreate(
      [](void *param)
      {
        static_cast<UsbHandler *>(param)->rx_dispatch_task();
      },
      "usb_rx_dispatch", 4096, this, 9, &rx_task_handle);
  assert(task_created == pdTRUE);
}

UsbHandler::~UsbHandler()
{
  if (rx_task_handle)
  {
    vTaskDelete(rx_task_handle);
  }

  if (rx_queue)
  {
    RxMessage message;
    while (xQueueReceive(rx_queue, &message, 0) == pdTRUE)
    {
      free(message.data);
    }
    vQueueDelete(rx_queue);
  }

  vSemaphoreDelete(device_disconnected_sem);
}

void UsbHandler::usb_loop()
{
  while (!s_usb_host_installed || !s_cdc_acm_installed)
  {
    // Install USB Host driver. Should only be called once in entire application
    if (!s_usb_host_installed)
    {
      ESP_LOGI(TAG, "Installing USB Host");
      usb_host_config_t host_config = {};
      host_config.skip_phy_setup = false;
      host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

      esp_err_t err = usb_host_install(&host_config);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
      {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        ledIndicator->setState(LedState::ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      if (err == ESP_ERR_INVALID_STATE)
      {
        ESP_LOGW(TAG, "USB Host already installed, reusing existing host stack");
      }

      s_usb_host_installed = true;
    }

    if (!s_usb_lib_task_started)
    {
      // Create a task that will handle USB library events
      BaseType_t task_created = xTaskCreate(
          [](void *param)
          {
            static_cast<UsbHandler *>(param)->usb_lib_task(param);
          },
          "usb_lib", 4096, this, 10, NULL);
      assert(task_created == pdTRUE);
      s_usb_lib_task_started = true;
    }

    if (!s_cdc_acm_installed)
    {
      ESP_LOGI(TAG, "Installing CDC-ACM driver");
      cdc_acm_host_driver_config_t cdc_acm_config;
      memset(&cdc_acm_config, 0, sizeof(cdc_acm_config));
      cdc_acm_config.driver_task_stack_size = 4096;
      cdc_acm_config.driver_task_priority = 10;
      cdc_acm_config.xCoreID = 0;

      esp_err_t err = cdc_acm_host_install(&cdc_acm_config);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
      {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        ledIndicator->setState(LedState::ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      if (err == ESP_ERR_INVALID_STATE)
      {
        ESP_LOGW(TAG, "CDC-ACM driver already installed, reusing existing driver");
      }

      s_cdc_acm_installed = true;
    }
  }

// Do everything else in a loop, so we can demonstrate USB device reconnections
  while (true)
  {
    cdc_acm_host_device_config_t dev_config = {
      .connection_timeout_ms = 5000,
      .out_buffer_size = 512,
      .in_buffer_size = 512,
      .event_cb = [](const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
        { static_cast<UsbHandler *>(user_ctx)->handle_event(event, user_ctx); },

      .data_cb = [](const uint8_t *data, size_t data_len, void *user_arg)
        { return static_cast<UsbHandler *>(user_arg)->handle_rx(data, data_len, user_arg); },
      .user_arg = this,
    };

    ESP_LOGI(TAG, "Opening Native CDC-ACM device (CH343/Standard ACM)...");
    
    // 1. Instantiate a new native CDC-ACM device object
    auto new_device = std::make_unique<CdcAcmDevice>();

    // 2. Call the .open() instance method on that object
    esp_err_t open_err = new_device->open(0x1A86, 0x55D3, 1, &dev_config);

    if (open_err == ESP_OK)
    {
      // 3. Move ownership to your class-level `vcp` pointer if successful
      vcp = std::move(new_device);
    }
    else
    {
      ESP_LOGE(TAG, "Failed to open CDC-ACM device: %s, retrying...", esp_err_to_name(open_err));
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    ledIndicator->setState(LedState::USB_CONNECTED);

    if (connection_callback) {
        connection_callback(true);
    }

    ESP_LOGI(TAG, "Setting up line coding");
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = BAUDRATE,
        .bCharFormat = STOP_BITS,
        .bParityType = PARITY,
        .bDataBits = DATA_BITS,
    };
    
    // These standard CDC-ACM commands will now work perfectly because the CH343 supports them natively!
    esp_err_t target_err = vcp->line_coding_set(&line_coding);
    if (target_err != ESP_OK) {
        ESP_LOGW(TAG, "Device rejected standard line coding configuration (%s). Proceeding anyway...", esp_err_to_name(target_err));
    }
    target_err = vcp->set_control_line_state(true, true);
    if (target_err != ESP_OK) {
        ESP_LOGW(TAG, "Device rejected standard control line state configuration (%s). Proceeding anyway...", esp_err_to_name(target_err));
    }

    ESP_LOGI(TAG, "CDC-ACM device connected. Waiting for disconnection...");
    xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);

    ledIndicator->setState(LedState::IDLE);

    if (connection_callback) {
        connection_callback(false);
    }

    ESP_LOGI(TAG, "CDC-ACM device disconnected. Cleaning up...");
    vcp.reset();
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

void UsbHandler::set_rx_callback(std::function<void(const uint8_t* data, size_t len)> cb)
{
    rx_callback = cb;
}

void UsbHandler::set_connection_callback(std::function<void(bool connected)> cb)
{
    connection_callback = cb;
}