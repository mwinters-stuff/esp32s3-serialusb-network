#ifndef _USB_HANDLER_H
#define _USB_HANDLER_H

#include <memory>
#include <functional>
#include <string>

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

#include "led_indicator.h"

class UsbHandler
{
private:
  // Callback for received data
  std::function<void(const uint8_t* data, size_t len)> rx_callback;
  // Callback for connection status changes
  std::function<void(bool connected)> connection_callback;

  SemaphoreHandle_t device_disconnected_sem;
  std::unique_ptr<CdcAcmDevice> vcp;
  std::shared_ptr<LedIndicator> ledIndicator;

  bool handle_rx(const uint8_t *data, size_t data_len, void *arg);
  void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
  void usb_lib_task(void *arg);

public:
  UsbHandler(std::shared_ptr<LedIndicator> led);
  virtual ~UsbHandler();

  void usb_loop();
  esp_err_t tx_blocking(uint8_t *data, size_t len);
  void set_rx_callback(std::function<void(const uint8_t* data, size_t len)> cb);
  void set_connection_callback(std::function<void(bool connected)> cb);
  bool isConnected() { return vcp != nullptr; }
};

#endif
