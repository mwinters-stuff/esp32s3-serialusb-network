#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <memory>

#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "usb/cdc_acm_host.h"

#include "http-server.h"

static const char *TAG = "HTTP-SERVER";

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

esp_err_t HttpServer::ota_update_handler(httpd_req_t *req)
{
    char url[256] = {0};
    int len = httpd_req_recv(req, url, sizeof(url) - 1);
    if (len <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No URL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Starting OTA from URL: %s", url);

    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.timeout_ms = 10000;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK)
    {
        httpd_resp_sendstr(req, "OTA Success! Rebooting...");
        esp_restart();
    }
    else
    {
        httpd_resp_sendstr(req, "OTA Failed!");
    }
    return ret;
}

esp_err_t HttpServer::upload_page_handler(httpd_req_t *req)
{
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

esp_err_t HttpServer::firmware_upload_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *ota_write_buf = (char *)malloc(4096); // Use a smaller buffer for writing
    if (!ota_write_buf)
    {
        esp_ota_end(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory for OTA buffer");
        return ESP_FAIL;
    }

    int received_bytes = 0;
    int remaining_bytes = req->content_len;

    while (remaining_bytes > 0)
    {
        int recv_len = httpd_req_recv(req, ota_write_buf, std::min(remaining_bytes, 4096));
        if (recv_len <= 0)
        {
            free(ota_write_buf);
            esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error during OTA");
            return ESP_FAIL;
        }
        ret = esp_ota_write(ota_handle, ota_write_buf, recv_len);
        received_bytes += recv_len;
        remaining_bytes -= recv_len;
    }
    free(ota_write_buf);
    if (ret != ESP_OK)
    {
        esp_ota_end(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }
    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }
    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Firmware uploaded! Rebooting...");
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

httpd_handle_t HttpServer::start()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
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

        httpd_uri_t term_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = HTTP_HANDLER(HttpServer, terminal_page_handler),
            .user_ctx = this};
        httpd_register_uri_handler(server, &term_uri);

        httpd_uri_t ota_uri = {
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = HTTP_HANDLER(HttpServer, ota_update_handler),
            .user_ctx = this};
        httpd_register_uri_handler(server, &ota_uri);

        httpd_uri_t upload_uri = {
            .uri = "/upload.html",
            .method = HTTP_GET,
            .handler = HTTP_HANDLER(HttpServer, upload_page_handler),
            .user_ctx = this};
        httpd_register_uri_handler(server, &upload_uri);

        httpd_uri_t fw_upload_post_uri = {
            .uri = "/upload",
            .method = HTTP_POST,
            .handler = HTTP_HANDLER(HttpServer, firmware_upload_handler),
            .user_ctx = this};
        httpd_register_uri_handler(server, &fw_upload_post_uri);
    }
    return server;
}
