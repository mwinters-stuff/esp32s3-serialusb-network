#ifndef _HTTP_SERVER_H
#define _HTTP_SERVER_H

#include <vector>
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
  httpd_handle_t server = NULL;
  std::vector<int> ws_clients;
  SemaphoreHandle_t ws_clients_mutex;
  bool isUSBConnected = false;

  void broadcast(const uint8_t *data, size_t len);

  static void ping_task_wrapper(void *arg);

  esp_err_t firmware_upload_handler(httpd_req_t *req);
  esp_err_t terminal_page_handler(httpd_req_t *req);
  esp_err_t websocket_handler(httpd_req_t *req);
  esp_err_t fs_upload_handler(httpd_req_t *req);
  esp_err_t upload_page_handler(httpd_req_t *req);

  esp_err_t login_page_handler(httpd_req_t *req);
  esp_err_t login_post_handler(httpd_req_t *req);

  bool is_authenticated(httpd_req_t *req);
public:
  HttpServer(std::shared_ptr<UsbHandler> usbHandler);
  virtual ~HttpServer();

  httpd_handle_t start();
  void ping_task();
};

#endif
