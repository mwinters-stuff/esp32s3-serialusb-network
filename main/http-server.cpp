#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <memory>

#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include "usb/cdc_acm_host.h"

#include "http-server.h"
#include "config.h"

static const char *TAG = "HTTP";

extern char serial_buffer[1024];
extern size_t serial_buffer_len;
extern SemaphoreHandle_t serial_buffer_mutex;

extern std::unique_ptr<CdcAcmDevice> vcp;

// template <typename T>
// auto make_handler(T* instance, esp_err_t (T::*method)(httpd_req_t*)) {
//     return [method](httpd_req_t* req) -> esp_err_t {
//         auto* self = static_cast<T*>(req->user_ctx);
//         return (self->*method)(req);
//     };
// }

#define HTTP_HANDLER(CLASS, METHOD) \
  [](httpd_req_t *req) -> esp_err_t { \
        auto* self = static_cast<CLASS*>(req->user_ctx); \
        return self->METHOD(req); }

bool HttpServer::is_authenticated(httpd_req_t *req)
{
    char cookie_buf[64];
    size_t buf_len = sizeof(cookie_buf);

    // Check for cookie "session=valid"
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, buf_len) == ESP_OK) {
        if (strstr(cookie_buf, "session=valid") != NULL) {
            return true;
        }
    }
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

esp_err_t HttpServer::serial_post_handler(httpd_req_t *req)
{
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf));
  if (ret > 0)
  {
    // Echo back to USB serial (replace with your VCP tx function)
    // For example: vcp->tx_blocking((uint8_t*)buf, ret);
    // For demo, just print:
    printf("Web input: %.*s\n", ret, buf);
    if (usbHandler)
    {
      usbHandler->tx_blocking((uint8_t *)buf, ret);
    }
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// HTTP handler to serve serial buffer
esp_err_t HttpServer::serial_get_handler(httpd_req_t *req)
{
  return usbHandler->serial_get_handler(req);
}

esp_err_t HttpServer::status_get_handler(httpd_req_t *req)
{
    if (usbHandler) {
        bool connected = usbHandler->isConnected();
        httpd_resp_set_type(req, "application/json");
        char resp[32];
        snprintf(resp, sizeof(resp), "{\"connected\": %s}", connected ? "true" : "false");
        httpd_resp_send(req, resp, strlen(resp));
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "USB handler not available");
    }
    return ESP_OK;
}

static void littlefs_unmount_if_mounted(void) {
    // Unregister; if not mounted this returns ESP_ERR_NOT_FOUND which we ignore.
    esp_err_t er = esp_vfs_littlefs_unregister("littlefs");
    if ( er == ESP_OK ) {
        ESP_LOGI(TAG, "LittleFS unmounted");
    } else {
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

    esp_err_t err;

    // Find LittleFS partition by label or subtype.
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LittleFS partition not found");
        return ESP_FAIL;
    }

    const size_t img_size = req->content_len;
    if (img_size == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (img_size > p->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Image exceeds LittleFS partition size", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Unmount before writing
    littlefs_unmount_if_mounted();

    ESP_LOGI(TAG, "Erasing LittleFS partition at 0x%08x, size %u", p->address, (unsigned)p->size);
    err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        return ESP_FAIL;
    }

    // Heap buffer (keep task stack small)
    const size_t BUF = 4096;
    uint8_t *buf = (uint8_t *)malloc(BUF);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    size_t remaining = img_size;
    size_t written   = 0;

    ESP_LOGI(TAG, "Writing LittleFS image (%u bytes) ...", (unsigned)img_size);

    while (remaining > 0) {
        size_t to_read = remaining > BUF ? BUF : remaining;
        int r = httpd_req_recv(req, (char*)buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                // retry same iteration
                continue;
            }
            ESP_LOGE(TAG, "recv error: %d", r);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        // Bounds guard (shouldn’t be needed due to the remaining check, but belt & braces)
        if (written + r > p->size) {
            free(buf);
            httpd_resp_set_status(req, "413 Payload Too Large");
            httpd_resp_send(req, "Image exceeds LittleFS partition size", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        err = esp_partition_write(p, written, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition write failed at off %u len %d: %s",
                     (unsigned)written, r, esp_err_to_name(err));
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition write failed");
            return ESP_FAIL;
        }

        written   += r;
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
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char password[64];
    // Body is urlencoded, e.g. "password=mypass"
    if (httpd_query_key_value(buf, "password", password, sizeof(password)) == ESP_OK) {
        if (strcmp(password, HTTP_PASSWORD) == 0) {
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

httpd_handle_t HttpServer::start()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.stack_size = 12 * 1024; // ← bump stack (12–16 KB is safe)
  config.recv_wait_timeout = 30; // seconds (optional)
  config.send_wait_timeout = 30;
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) == ESP_OK)
  {
    httpd_uri_t serial_uri = {
        .uri = "/serial",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, serial_get_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &serial_uri);

    httpd_uri_t serial_post_uri = {
        .uri = "/serial",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, serial_post_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &serial_post_uri);

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, status_get_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t term_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, terminal_page_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &term_uri);

    httpd_uri_t fw_upload_post_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, firmware_upload_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &fw_upload_post_uri);

    httpd_uri_t fw_uploadfs_post_uri = {
        .uri = "/uploadfs",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, fs_upload_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &fw_uploadfs_post_uri);

    httpd_uri_t upload_uri = {
        .uri = "/upload.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, upload_page_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &upload_uri);

    httpd_uri_t login_uri = {
        .uri = "/login.html",
        .method = HTTP_GET,
        .handler = HTTP_HANDLER(HttpServer, login_page_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &login_uri);

    httpd_uri_t login_post_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = HTTP_HANDLER(HttpServer, login_post_handler),
        .user_ctx = this};
    httpd_register_uri_handler(server, &login_post_uri);

  }
  return server;
}

esp_err_t HttpServer::upload_page_handler(httpd_req_t *req)
{

  if (!is_authenticated(req)) {
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
