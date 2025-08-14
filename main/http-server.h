#ifndef _HTTP_SERVER_H
#define _HTTP_SERVER_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "usb/cdc_acm_host.h"

#include "usb-handler.h"

class HttpServer
{
private:
  std::shared_ptr<UsbHandler> usbHandler;
  esp_err_t ota_update_handler(httpd_req_t *req);
  esp_err_t upload_page_handler(httpd_req_t *req);
  esp_err_t firmware_upload_handler(httpd_req_t *req);
  esp_err_t terminal_page_handler(httpd_req_t *req);
  esp_err_t serial_post_handler(httpd_req_t *req);
  esp_err_t serial_get_handler(httpd_req_t *req);

public:
  HttpServer(std::shared_ptr<UsbHandler> usbHandler) : usbHandler(usbHandler) {};
  virtual ~HttpServer() {};

  httpd_handle_t start();
};

#endif
