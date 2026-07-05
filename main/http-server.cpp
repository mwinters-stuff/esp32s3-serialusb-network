#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <esp_http_server.h>

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "WebSocket support is not enabled. Please run 'idf.py menuconfig', go to Component config -> HTTP Server, and enable [ ] Enable Websocket support."
#endif

#include <esp_https_ota.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <usb/cdc_acm_host.h>

#include "config.h"
#include "http-server.h"

static const char *TAG = "HTTP";

namespace
{
constexpr size_t MAX_RECENT_LINE_MESSAGES = 64;

struct WsSendAsyncContext
{
  int fd;
  std::string *message;
};

std::string json_escape(const uint8_t *data, size_t len)
{
  std::string escaped;
  escaped.reserve(len + 16);

  for (size_t i = 0; i < len; ++i)
  {
    const unsigned char ch = data[i];
    switch (ch)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (ch < 0x20 || ch >= 0x80)
      {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04X", ch);
        escaped += buf;
      }
      else
      {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }

  return escaped;
}

}

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
  if (len == 0)
  {
    return;
  }

  std::string payload = "{\"type\":\"line\",\"data\":\"";
  payload += json_escape(data, len);
  payload += "\"}";

  ESP_LOGD(TAG, "Sending terminal line: %s", payload.c_str());

  if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) == pdTRUE)
  {
    recent_line_messages.push_back(payload);
    while (recent_line_messages.size() > MAX_RECENT_LINE_MESSAGES)
    {
      recent_line_messages.pop_front();
    }
    xSemaphoreGive(ws_clients_mutex);
  }

  broadcast_text_message(payload);
}

void HttpServer::broadcast_text_message(const std::string &message)
{
  if (message.empty())
  {
    return;
  }

  if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) != pdTRUE)
  {
    ESP_LOGW(TAG, "Broadcasting no semiphore");
    return;
  }

  // Using an iterator-based loop is safer for erasing elements.
  for (auto it = ws_clients.begin(); it != ws_clients.end();)
  {
    int fd = *it;
    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(message.data()));
    ws_pkt.len = message.size();
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_send_data(this->server, fd, &ws_pkt);
    if (ret != ESP_OK)
    {
      if (ret == ESP_ERR_NO_MEM)
      {
        ESP_LOGW(TAG, "WS send queue full for fd %d. Dropping packet.", fd);
        ++it; // Move to the next client
      }
      else
      {
        ESP_LOGW(TAG, "httpd_ws_send_data failed with %d on fd %d, removing client", ret, fd);
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
    std::deque<std::string> replay_messages;
    if (xSemaphoreTake(ws_clients_mutex, portMAX_DELAY) == pdTRUE)
    {
      // Add the client only on the initial GET request.
      // Check for duplicates in case of rapid reconnects.
      if (std::find(ws_clients.begin(), ws_clients.end(), fd) == ws_clients.end())
      {
        ws_clients.push_back(fd);
      }
      replay_messages = recent_line_messages;
      xSemaphoreGive(ws_clients_mutex);

      if (usbHandler)
      {
        isUSBConnected = usbHandler->isConnected();
        ESP_LOGI(TAG, "New client connected, USB status: %s", isUSBConnected ? "connected" : "disconnected");
        std::string resp = isUSBConnected ? "{\"type\":\"status\",\"connected\":true}" : "{\"type\":\"status\",\"connected\":false}";

        httpd_ws_frame_t status_pkt = {};
        status_pkt.payload = (uint8_t *)resp.data();
        status_pkt.len = resp.size();
        status_pkt.type = HTTPD_WS_TYPE_TEXT;

        esp_err_t status_ret = httpd_ws_send_frame(req, &status_pkt);
        if (status_ret != ESP_OK)
        {
          ESP_LOGW(TAG, "Initial status send failed on fd %d with %d", fd, status_ret);
        }
      }

      for (const auto &replay_message : replay_messages)
      {
        httpd_ws_frame_t replay_pkt = {};
        replay_pkt.payload = (uint8_t *)replay_message.data();
        replay_pkt.len = replay_message.size();
        replay_pkt.type = HTTPD_WS_TYPE_TEXT;

        esp_err_t replay_ret = httpd_ws_send_frame(req, &replay_pkt);
        if (replay_ret != ESP_OK)
        {
          ESP_LOGW(TAG, "Replay line send failed on fd %d with %d", fd, replay_ret);
          break;
        }
      }

      return ESP_OK;
    }
    // Do not return here. The handler must continue to process frames.
  }

  // Output-only mode for stability: ignore inbound websocket frames for now.
  // Re-enabled inbound text path so browser can transmit to the USB device.
  httpd_ws_frame_t ws_pkt = {};
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "WS recv (header) failed on fd %d with %d", httpd_req_to_sockfd(req), ret);
    return ret;
  }

  if (ws_pkt.len == 0)
  {
    return ESP_OK;
  }

  std::vector<uint8_t> payload(ws_pkt.len + 1, 0);
  ws_pkt.payload = payload.data();
  ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "WS recv (payload) failed on fd %d with %d", httpd_req_to_sockfd(req), ret);
    return ret;
  }

  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY)
  {
    ESP_LOGI(TAG, "WS inbound frame type=%d len=%u", ws_pkt.type, (unsigned)ws_pkt.len);
    if (usbHandler && usbHandler->isConnected())
    {
      esp_err_t tx_ret = usbHandler->tx_blocking(payload.data(), ws_pkt.len);
      if (tx_ret != ESP_OK)
      {
        ESP_LOGW(TAG, "USB tx_blocking failed: %s", esp_err_to_name(tx_ret));
      }
    }
    else
    {
      ESP_LOGW(TAG, "Dropping WS outbound data: USB not connected");
    }
  }

  return ESP_OK;
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
          ledIndicator->setState(LedState::NETWORK_CONNECTED);
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
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &term_uri);

    // URI handler for WebSocket connection
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, websocket_handler),
        .user_ctx = this,
        .is_websocket = true,
      .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &ws_uri);

    // URI handler for firmware upload
    httpd_uri_t fw_upload_post_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, firmware_upload_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &fw_upload_post_uri);

    // URI handler for filesystem upload
    httpd_uri_t fw_uploadfs_post_uri = {
        .uri = "/uploadfs",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, fs_upload_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &fw_uploadfs_post_uri);

    // URI handler for the upload page
    httpd_uri_t upload_uri = {
        .uri = "/upload.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, upload_page_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &upload_uri);

    // URI handler for the login page
    httpd_uri_t login_uri = {
        .uri = "/login.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, login_page_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &login_uri);

    // URI handler for login form submission
    httpd_uri_t login_post_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, login_post_handler),
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL};
    httpd_register_uri_handler(this->server, &login_post_uri);

    // Disabled custom ping task for now.
    // Browser PONG/control-frame handling on this ESP-IDF websocket path was destabilizing
    // long-lived output streaming, so keep the connection passive while we validate RX flow.
  }

  // Set up callbacks for USB events
  if (usbHandler)
  {
    usbHandler->set_rx_callback(
        [this](const uint8_t *data, size_t len)
        { this->broadcast(data, len); });
    usbHandler->set_connection_callback([this](bool connected)
                                        {
      this->broadcast_text_message(connected ? "{\"type\":\"status\",\"connected\":true}" : "{\"type\":\"status\",\"connected\":false}"); });
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
