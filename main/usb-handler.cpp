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
#include <esp_private/cdc_host_common.h>
#include <usb/usb_host.h>
#include <usb/vcp_ch34x.h>
#include <usb/vcp.hpp>
// #include "usb/vcp_cp210x.hpp"
// #include "usb/vcp_ftdi.hpp"

#include <esp_http_server.h>

#include "usb-handler.h"
#include "local-ch34x-device.h"
static const char *TAG = "VCP";

using namespace esp_usb;

namespace
{
  constexpr size_t RX_LINE_MAX_LEN = 512;
  constexpr TickType_t RX_FLUSH_TIMEOUT_TICKS = pdMS_TO_TICKS(50);
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
  ESP_LOGI(TAG, "Received %d bytes of data", (int)data_len);
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
  TickType_t last_rx_tick = 0;

  while (true)
  {
    if (xQueueReceive(rx_queue, &message, RX_FLUSH_TIMEOUT_TICKS) != pdTRUE)
    {
      if (!rx_line_buffer.empty() && last_rx_tick != 0 && (xTaskGetTickCount() - last_rx_tick) >= RX_FLUSH_TIMEOUT_TICKS)
      {
        ESP_LOGI(TAG, "RX partial line timeout, flushing %u buffered bytes", (unsigned)rx_line_buffer.size());
        flush_rx_line(false);
        last_rx_tick = 0;
      }
      continue;
    }
    ESP_LOGI(TAG, "Dispatching %d bytes of RX data", (int)message.len);
    last_rx_tick = xTaskGetTickCount();

    for (size_t i = 0; i < message.len; ++i)
    {
      const char ch = static_cast<char>(message.data[i]);

      if (ch == '\n')
      {
        flush_rx_line(true);
        last_rx_tick = 0;
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
        last_rx_tick = 0;
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

  rx_line_buffer.clear();
}

UsbHandler::UsbHandler(std::shared_ptr<LedIndicator> led) : rx_queue(NULL), rx_task_handle(NULL), using_vendor_ch34x_driver(false), ledIndicator(led)
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
      cdc_acm_config.new_dev_cb = [](usb_device_handle_t usb_dev)
      {
        const usb_device_desc_t *device_desc = NULL;
        if (usb_host_get_device_descriptor(usb_dev, &device_desc) == ESP_OK && device_desc != NULL)
        {
          ESP_LOGI(TAG, "CDC new device: VID=0x%04X PID=0x%04X", device_desc->idVendor, device_desc->idProduct);
        }
        else
        {
          ESP_LOGW(TAG, "CDC new device connected, but descriptor read failed");
        }
      };

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

    ESP_LOGI(TAG, "Opening CH34x VCP device...");

    std::unique_ptr<CdcAcmDevice> new_device;
    esp_err_t open_err = ESP_ERR_NOT_FOUND;

    using_vendor_ch34x_driver = false;

    const uint8_t candidate_interfaces[] = {0, 1};
    const uint16_t candidate_pids[] = {CH34X_PID_AUTO, CH340_PID_1, CH340_PID, CH341_PID, 0x55D3};

    for (uint8_t interface_idx : candidate_interfaces)
    {
      for (uint16_t pid : candidate_pids)
      {
        ESP_LOGI(TAG, "Trying CH34x vendor-specific open: pid=0x%04X interface=%u", pid, interface_idx);
        try
        {
          new_device = std::make_unique<LocalCh34xDevice>(pid, &dev_config, interface_idx);
          open_err = ESP_OK;
          using_vendor_ch34x_driver = true;
          ESP_LOGI(TAG, "Opened CH34x VCP device with vendor-specific driver (pid=0x%04X interface=%u)", pid, interface_idx);
          break;
        }
        catch (esp_err_t err)
        {
          open_err = err;
        }
        catch (...)
        {
          open_err = ESP_FAIL;
        }
      }

      if (open_err == ESP_OK)
      {
        break;
      }
    }

    if (open_err != ESP_OK)
    {
      for (uint8_t interface_idx : candidate_interfaces)
      {
        for (uint16_t pid : candidate_pids)
        {
          if (pid == CH34X_PID_AUTO)
          {
            continue;
          }

          auto generic_device = std::make_unique<CdcAcmDevice>();
          ESP_LOGI(TAG, "Trying generic CDC-ACM open: vid=0x%04X pid=0x%04X interface=%u", NANJING_QINHENG_MICROE_VID, pid, interface_idx);
          open_err = generic_device->open(NANJING_QINHENG_MICROE_VID, pid, interface_idx, &dev_config);
          if (open_err == ESP_OK)
          {
            ESP_LOGI(TAG, "Opened device with generic CDC-ACM driver (pid=0x%04X interface=%u)", pid, interface_idx);
            using_vendor_ch34x_driver = false;
            new_device = std::move(generic_device);
            break;
          }
        }

        if (open_err == ESP_OK)
        {
          break;
        }
      }
    }

    if (open_err == ESP_OK)
    {
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
    
    esp_err_t target_err = vcp->line_coding_set(&line_coding);
    if (target_err != ESP_OK) {
        ESP_LOGW(TAG, "Device rejected standard line coding configuration (%s). Proceeding anyway...", esp_err_to_name(target_err));
    } else {
      ESP_LOGI(TAG, "Configured line coding: %d baud, data=%d parity=%d stop=%d", BAUDRATE, DATA_BITS, PARITY, STOP_BITS);
    }
    target_err = vcp->set_control_line_state(true, true);
    if (target_err != ESP_OK) {
        ESP_LOGW(TAG, "Device rejected standard control line state configuration (%s). Proceeding anyway...", esp_err_to_name(target_err));
    } else {
      ESP_LOGI(TAG, "Configured control line state: DTR=1 RTS=1");
    }

    ESP_LOGI(TAG, "CDC-ACM device connected. Waiting for disconnection...");
    xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);

    ledIndicator->setState(LedState::NETWORK_CONNECTED);

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