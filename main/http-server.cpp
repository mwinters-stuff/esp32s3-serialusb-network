#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <memory>
#include <vector>

#include "esp_http_server.h"

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "WebSocket support is not enabled. Please run 'idf.py menuconfig', go to Component config -> HTTP Server, and enable [ ] Enable Websocket support."
#endif

#include "esp_https_ota.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "usb/cdc_acm_host.h"

#include "config.h"
#include "http-server.h"

static const char *TAG = "HTTP";

HttpServer::HttpServer(std::shared_ptr<UsbHandler> usbHandler, std::shared_ptr<LedIndicator> led) : usbHandler(usbHandler), ledIndicator(led)
{
  ws_clients_mutex = xSemaphoreCreateMutex();
  assert(ws_clients_mutex);
}

HttpServer::~HttpServer()
{
  if (ws_clients_mutex)
  {
    vSemaphoreDelete(ws_clients_mutex);
  }
}

void HttpServer::ping_task()
{
  // This is a simple keep-alive mechanism to prevent the WebSocket from timing out.
  httpd_ws_frame_t ping_frame = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_PING,
      .payload = NULL,
      .len = 0};

  while (true)
  {
    // Wait for 10 seconds. This must be less than the httpd recv_wait_timeout.
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) != pdTRUE)
    {
      continue;
    }

    // Using an iterator-based loop is safer for erasing elements.
    for (auto it = ws_clients.begin(); it != ws_clients.end();)
    {
      esp_err_t ret = httpd_ws_send_frame_async(this->server, *it, &ping_frame);
      if (ret != ESP_OK)
      {
        ESP_LOGW(TAG, "Ping failed for fd %d with error %d, removing client", *it, ret);
        it = ws_clients.erase(it);
      }
      else
      {
        ++it;
      }
    }
    xSemaphoreGive(ws_clients_mutex);
  }
}

#define HTTP_HANDLER(CLASS, METHOD) \
  [](httpd_req_t *req) -> esp_err_t { \
        auto* self = static_cast<CLASS*>(req->user_ctx); \
        return self->METHOD(req); }

bool HttpServer::is_authenticated(httpd_req_t *req)
{
  char cookie_buf[64];

  // Check for cookie "session=valid"
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) == ESP_OK)
  {
    if (strstr(cookie_buf, "session=valid") != NULL)
    {
      ESP_LOGI(TAG, "Authenticated");
      return true;
    }
  }
  else
  {
    ESP_LOGI(TAG, "No cookie found");
  }
  ESP_LOGW(TAG, "Unauthenticated");
  return false;
}

esp_err_t HttpServer::firmware_upload_handler(httpd_req_t *req)
{
  if (!is_authenticated(req))
  {
    ESP_LOGW(TAG, "Unauthenticated firmware upload attempt");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Not authenticated");
    return ESP_FAIL;
  }

  if (ledIndicator) ledIndicator->setState(LedState::UPLOADING);

  esp_err_t err;
  const size_t content_len = req->content_len;

  const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
  if (!update)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Writing OTA to partition subtype %d at offset 0x%08x",
           update->subtype, update->address);

  esp_ota_handle_t ota = 0;
  err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return ESP_FAIL;
  }

  // Allocate receive buffer on HEAP (not on the task stack!)
  const size_t BUF_SZ = 4096; // 4 KB chunks is fine
  uint8_t *buf = (uint8_t *)malloc(BUF_SZ);
  if (!buf)
  {
    ESP_LOGE(TAG, "malloc failed");
    esp_ota_end(ota);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    return ESP_FAIL;
  }

  size_t remaining = content_len;
  while (remaining > 0)
  {
    const size_t to_read = remaining > BUF_SZ ? BUF_SZ : remaining;
    int r = httpd_req_recv(req, (char *)buf, to_read);
    if (r <= 0)
    {
      if (r == HTTPD_SOCK_ERR_TIMEOUT)
      {
        ESP_LOGW(TAG, "recv timeout, retrying...");
        continue; // allow the loop to retry
      }
      ESP_LOGE(TAG, "recv error: %d", r);
      free(buf);
      esp_ota_end(ota);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
      return ESP_FAIL;
    }

    err = esp_ota_write(ota, buf, r);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      free(buf);
      esp_ota_end(ota);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write fail");
      return ESP_FAIL;
    }

    remaining -= r;
  }

  free(buf);

  err = esp_ota_end(ota);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end fail");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot fail");
    return ESP_FAIL;
  }

  httpd_resp_set_hdr(req, "Connection", "close"); // avoid keep-alive issues
  httpd_resp_sendstr(req, "OK");

  ESP_LOGI(TAG, "OTA update complete (%d bytes). Rebooting...", (int)content_len);
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;
}

esp_err_t HttpServer::terminal_page_handler(httpd_req_t *req)
{
  FILE *f = fopen("/littlefs/terminal.html", "r");
  if (!f)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  char buf[1024];
  size_t read_bytes;
  while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0)
  {
    httpd_resp_send_chunk(req, buf, read_bytes);
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0); // End response
  return ESP_OK;
}

void HttpServer::broadcast(const uint8_t *data, size_t len)
{
  if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Broadcasting no semiphore");
    return;
  }

  // Using an iterator-based loop is safer for erasing elements.
  for (auto it = ws_clients.begin(); it != ws_clients.end();)
  {
    int fd = *it;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)data;
    ws_pkt.len = len;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT; // Or BINARY if you prefer

    // Asynchronous send.
    ESP_LOGI(TAG, "Broadcasting to %d ", fd);

    esp_err_t ret = httpd_ws_send_frame_async(this->server, fd, &ws_pkt);
    if (ret != ESP_OK)
    {
      // ESP_ERR_NO_MEM (257) means the async send queue is full. This is a transient error.
      // We should not disconnect the client. For this application, dropping the packet is
      // acceptable to avoid blocking the USB handler.
      if (ret == ESP_ERR_NO_MEM)
      {
        ESP_LOGW(TAG, "WS async send queue full for fd %d. Dropping packet.", fd);
        ++it; // Move to the next client
      }
      else
      {
        // Any other error indicates a client that is no longer valid.
        ESP_LOGW(TAG, "httpd_ws_send_frame_async failed with %d on fd %d, removing client", ret, fd);
        it = ws_clients.erase(it);
      }
    }
    else
    {
      ++it;
    }
  }

  xSemaphoreGive(ws_clients_mutex);
}

esp_err_t HttpServer::websocket_handler(httpd_req_t *req)
{
  // The WebSocket handler is called once when the client connects.
  // It is responsible for the entire lifecycle of the connection.
  if (req->method == HTTP_GET)
  {
    int fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "Handshake done, new WS client connected on fd %d", fd);
    if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) == pdTRUE)
    {
      // Add the client only on the initial GET request.
      // Check for duplicates in case of rapid reconnects.
      if (std::find(ws_clients.begin(), ws_clients.end(), fd) == ws_clients.end())
      {
        // If this is the first client, change the LED state
        if (ws_clients.empty() && ledIndicator)
        {
          ledIndicator->setState(LedState::WEB_TERMINAL_ACTIVE);
        }
        ws_clients.push_back(fd);
      }
      xSemaphoreGive(ws_clients_mutex);

      return ESP_OK;
    }
    // Do not return here. The handler must continue to process frames.
  }

  // Send initial connection status to the new client
  if (usbHandler)
  {
    if(isUSBConnected != usbHandler->isConnected())
    {
      isUSBConnected = usbHandler->isConnected();
      ESP_LOGI(TAG, "New client connected, USB status: %s", isUSBConnected ? "connected" : "disconnected");
      char resp[64];
      snprintf(resp, sizeof(resp), "{\"type\":\"status\", \"connected\": %s}", isUSBConnected ? "true" : "false");

      httpd_ws_frame_t ws_pkt;
      memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
      ws_pkt.payload = (uint8_t *)resp;
      ws_pkt.len = strlen(resp);
      ws_pkt.type = HTTPD_WS_TYPE_TEXT;

      // Asynchronous send to the new client.
      int fd = httpd_req_to_sockfd(req);
      httpd_ws_send_frame_async(this->server, fd, &ws_pkt);
    }
  }

  esp_err_t ret = ESP_OK;

  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = NULL;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  // This call blocks until a frame is received, a timeout occurs, or the connection is closed.
  ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK)
  {
    // A failure here usually means the client has disconnected.
    ESP_LOGI(TAG, "httpd_ws_recv_frame failed to get frame info with %d. Client disconnected.", ret);
    return ret; // Exit the loop to close the connection.
  }

  if (ws_pkt.len > 0)
  {
    buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
    if (buf == NULL)
    {
      ESP_LOGE(TAG, "Failed to calloc memory for websocket buffer");
      ret = ESP_ERR_NO_MEM;
      return ret; // Exit loop on memory error.
    }
    ws_pkt.payload = buf;

    // Get the full frame payload
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK)
    {
      ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
      free(buf);
      return ret; // Exit loop on receive error.
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
      if (usbHandler)
      {
        usbHandler->tx_blocking(buf, ws_pkt.len);
      }
    }
    free(buf);
  }

  return ret;
}

static void littlefs_unmount_if_mounted(void)
{
  // Unregister; if not mounted this returns ESP_ERR_NOT_FOUND which we ignore.
  esp_err_t er = esp_vfs_littlefs_unregister("littlefs");
  if (er == ESP_OK)
  {
    ESP_LOGI(TAG, "LittleFS unmounted");
  }
  else
  {
    ESP_LOGI(TAG, "LittleFS not mounted or already unmounted (err=0x%x)", er);
  }
}

esp_err_t HttpServer::fs_upload_handler(httpd_req_t *req)
{
  if (!is_authenticated(req))
  {
    ESP_LOGW(TAG, "Unauthenticated filesystem upload attempt");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Not authenticated");
    return ESP_FAIL;
  }

  if (ledIndicator) ledIndicator->setState(LedState::UPLOADING);

  esp_err_t err;

  // Find LittleFS partition by label or subtype.
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs");
  if (!p)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LittleFS partition not found");
    return ESP_FAIL;
  }

  const size_t img_size = req->content_len;
  if (img_size == 0)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    return ESP_FAIL;
  }
  if (img_size > p->size)
  {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_send(req, "Image exceeds LittleFS partition size", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  // Unmount before writing
  littlefs_unmount_if_mounted();

  ESP_LOGI(TAG, "Erasing LittleFS partition at 0x%08x, size %u", p->address, (unsigned)p->size);
  err = esp_partition_erase_range(p, 0, p->size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
    return ESP_FAIL;
  }

  // Heap buffer (keep task stack small)
  const size_t BUF = 4096;
  uint8_t *buf = (uint8_t *)malloc(BUF);
  if (!buf)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    return ESP_FAIL;
  }

  size_t remaining = img_size;
  size_t written = 0;

  ESP_LOGI(TAG, "Writing LittleFS image (%u bytes) ...", (unsigned)img_size);

  while (remaining > 0)
  {
    size_t to_read = remaining > BUF ? BUF : remaining;
    int r = httpd_req_recv(req, (char *)buf, to_read);
    if (r <= 0)
    {
      if (r == HTTPD_SOCK_ERR_TIMEOUT)
      {
        // retry same iteration
        continue;
      }
      ESP_LOGE(TAG, "recv error: %d", r);
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
      return ESP_FAIL;
    }

    // Bounds guard (shouldn’t be needed due to the remaining check, but belt & braces)
    if (written + r > p->size)
    {
      free(buf);
      httpd_resp_set_status(req, "413 Payload Too Large");
      httpd_resp_send(req, "Image exceeds LittleFS partition size", HTTPD_RESP_USE_STRLEN);
      return ESP_FAIL;
    }

    err = esp_partition_write(p, written, buf, r);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "partition write failed at off %u len %d: %s",
               (unsigned)written, r, esp_err_to_name(err));
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition write failed");
      return ESP_FAIL;
    }

    written += r;
    remaining -= r;
  }

  free(buf);

  ESP_LOGI(TAG, "LittleFS image written: %u bytes", (unsigned)written);

  // All done – respond and reboot
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, "OK");

  vTaskDelay(pdMS_TO_TICKS(800));
  esp_restart();
  return ESP_OK;
}

esp_err_t HttpServer::login_page_handler(httpd_req_t *req)
{
  FILE *f = fopen("/littlefs/login.html", "r");
  if (!f)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  char buf[1024];
  size_t read_bytes;
  while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0)
  {
    httpd_resp_send_chunk(req, buf, read_bytes);
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0); // End response
  return ESP_OK;
}

esp_err_t HttpServer::login_post_handler(httpd_req_t *req)
{
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0)
  {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
    {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  buf[ret] = '\0';

  char password[64];
  // Body is urlencoded, e.g. "password=mypass"
  if (httpd_query_key_value(buf, "password", password, sizeof(password)) == ESP_OK)
  {
    if (strcmp(password, HTTP_PASSWORD) == 0)
    {
      // Correct password. Set cookie and redirect.
      ESP_LOGI(TAG, "Login successful");
      httpd_resp_set_hdr(req, "Set-Cookie", "session=valid; Path=/; HttpOnly");
      httpd_resp_set_status(req, "302 Found");
      httpd_resp_set_hdr(req, "Location", "/upload.html");
      httpd_resp_send(req, NULL, 0);
      return ESP_OK;
    }
  }

  // Incorrect password or parse error. Redirect back to login with error.
  ESP_LOGW(TAG, "Failed login attempt");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login.html?error=1");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

void HttpServer::handle_client_close(int sockfd)
{
  if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) == pdTRUE)
  {
    auto it = std::find(ws_clients.begin(), ws_clients.end(), sockfd);
    if (it != ws_clients.end())
    {
      ws_clients.erase(it);
      ESP_LOGI(TAG, "WS client on fd %d disconnected. Remaining clients: %zu", sockfd, ws_clients.size());
      // If this was the last client, revert the state.
      if (ws_clients.empty() && ledIndicator)
      {
        // Revert to USB_CONNECTED or IDLE based on the current USB status
        if (usbHandler && usbHandler->isConnected())
        {
          ledIndicator->setState(LedState::USB_CONNECTED);
        }
        else
        {
          ledIndicator->setState(LedState::IDLE);
        }
      }
    }
    xSemaphoreGive(ws_clients_mutex);
  }
}

httpd_handle_t HttpServer::start()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.stack_size = 12 * 1024; // ← bump stack (12–16 KB is safe)
  config.recv_wait_timeout = 30; // seconds (optional)
  config.send_wait_timeout = 30;
  // config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 10;
  config.lru_purge_enable = true;

  // Set up a function to be called when a client socket is closed
  config.global_user_ctx = this;
  config.close_fn = [](httpd_handle_t hd, int sockfd) {
    auto *self = static_cast<HttpServer *>(httpd_get_global_user_ctx(hd));
    if (self)
    {
      self->handle_client_close(sockfd);
    }
  };

  if (httpd_start(&this->server, &config) == ESP_OK)
  {
    // URI handler for the terminal page
    httpd_uri_t term_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, terminal_page_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &term_uri);

    // URI handler for WebSocket connection
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, websocket_handler),
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = true, // Enable automatic handling of PONG
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &ws_uri);

    // URI handler for firmware upload
    httpd_uri_t fw_upload_post_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, firmware_upload_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &fw_upload_post_uri);

    // URI handler for filesystem upload
    httpd_uri_t fw_uploadfs_post_uri = {
        .uri = "/uploadfs",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, fs_upload_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &fw_uploadfs_post_uri);

    // URI handler for the upload page
    httpd_uri_t upload_uri = {
        .uri = "/upload.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, upload_page_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &upload_uri);

    // URI handler for the login page
    httpd_uri_t login_uri = {
        .uri = "/login.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, login_page_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &login_uri);

    // URI handler for login form submission
    httpd_uri_t login_post_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, login_post_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = NULL,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &login_post_uri);

    // Start a task to periodically send PING frames to all clients
    xTaskCreate(
        [](void *param)
        {
          static_cast<HttpServer *>(param)->ping_task();
        },
        "ws_ping_task", 4096, this, 5, NULL);
  }

  // Set up callbacks for USB events
  if (usbHandler)
  {
    usbHandler->set_rx_callback(
        [this](const uint8_t *data, size_t len)
        { this->broadcast(data, len); });
    usbHandler->set_connection_callback([this](bool connected)
                                        {
      char resp[64];
      snprintf(resp, sizeof(resp), "{\"type\":\"status\", \"connected\": %s}", connected ? "true" : "false");
      this->broadcast((uint8_t*)resp, strlen(resp)); });
  }
  return this->server;
}

esp_err_t HttpServer::upload_page_handler(httpd_req_t *req)
{

  if (!is_authenticated(req))
  {
    // Redirect to login page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
  }

  FILE *f = fopen("/littlefs/upload.html", "r");
  if (!f)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  char buf[1024];
  size_t read_bytes;
  while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0)
  {
    httpd_resp_send_chunk(req, buf, read_bytes);
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0); // End response
  return ESP_OK;
}
