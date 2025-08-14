#ifndef _USB_HANDLER_H
#define _USB_HANDLER_H

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

class UsbHandler
{
private:
  char serial_buffer[1024];
  size_t serial_buffer_len = 0;
  SemaphoreHandle_t serial_buffer_mutex;
  SemaphoreHandle_t device_disconnected_sem;
  std::unique_ptr<CdcAcmDevice> vcp;

  bool handle_rx(const uint8_t *data, size_t data_len, void *arg);
  void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
  void usb_lib_task(void *arg);

public:
  UsbHandler();
  virtual ~UsbHandler() {};

  void usb_loop();
  esp_err_t tx_blocking(uint8_t *data, size_t len);
  esp_err_t serial_get_handler(httpd_req_t *req);
};

#endif
