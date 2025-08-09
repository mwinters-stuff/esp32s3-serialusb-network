/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>

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

#include "esp_http_server.h"
#include "freertos/queue.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

using namespace esp_usb;

namespace
{
    static const char *TAG = "VCP";
    static SemaphoreHandle_t device_disconnected_sem;

    // Buffer for received data
    static char serial_buffer[1024];
    static size_t serial_buffer_len = 0;
    static SemaphoreHandle_t serial_buffer_mutex;

    static std::unique_ptr<CdcAcmDevice> vcp;

    esp_err_t ota_update_handler(httpd_req_t *req);
    esp_err_t upload_page_handler(httpd_req_t *req);
    esp_err_t firmware_upload_handler(httpd_req_t *req);

    esp_err_t terminal_page_handler(httpd_req_t *req)
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

    esp_err_t serial_post_handler(httpd_req_t *req)
    {
        char buf[128];
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret > 0)
        {
            // Echo back to USB serial (replace with your VCP tx function)
            // For example: vcp->tx_blocking((uint8_t*)buf, ret);
            // For demo, just print:
            printf("Web input: %.*s\n", ret, buf);
            if (vcp)
                vcp->tx_blocking((uint8_t *)buf, ret);
        }
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }

    // HTTP handler to serve serial buffer
    esp_err_t serial_get_handler(httpd_req_t *req)
    {
        xSemaphoreTake(serial_buffer_mutex, portMAX_DELAY);
        httpd_resp_send(req, serial_buffer, serial_buffer_len);
        xSemaphoreGive(serial_buffer_mutex);
        return ESP_OK;
    }

    httpd_handle_t start_webserver(void)
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        httpd_handle_t server = NULL;
        if (httpd_start(&server, &config) == ESP_OK)
        {
            httpd_uri_t serial_uri = {
                .uri = "/serial",
                .method = HTTP_GET,
                .handler = serial_get_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &serial_uri);

            httpd_uri_t serial_post_uri = {
                .uri = "/serial",
                .method = HTTP_POST,
                .handler = serial_post_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &serial_post_uri);

            httpd_uri_t term_uri = {
                .uri = "/",
                .method = HTTP_GET,
                .handler = terminal_page_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &term_uri);

            httpd_uri_t ota_uri = {
                .uri = "/ota",
                .method = HTTP_POST,
                .handler = ota_update_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &ota_uri);

            httpd_uri_t upload_uri = {
                .uri = "/upload.html",
                .method = HTTP_GET,
                .handler = upload_page_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &upload_uri);

            httpd_uri_t fw_upload_post_uri = {
                .uri = "/upload",
                .method = HTTP_POST,
                .handler = firmware_upload_handler,
                .user_ctx = NULL};
            httpd_register_uri_handler(server, &fw_upload_post_uri);
        }
        return server;
    }

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
    static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
    {
        // ESP_LOGI(TAG, "handle_rx called, data_len=%d", (int)data_len);
        printf("%.*s", (int)data_len, data);

        // Buffer the data for web interface
        xSemaphoreTake(serial_buffer_mutex, portMAX_DELAY);
        size_t copy_len = data_len;
        if (serial_buffer_len + copy_len > sizeof(serial_buffer))
        {
            // If buffer full, reset
            serial_buffer_len = 0;
            copy_len = data_len < sizeof(serial_buffer) ? data_len : sizeof(serial_buffer);
        }
        memcpy(serial_buffer + serial_buffer_len, data, copy_len);
        serial_buffer_len += copy_len;
        xSemaphoreGive(serial_buffer_mutex);

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
    static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
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
    static void usb_lib_task(void *arg)
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

    void wifi_init_sta()
    {
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *netif = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(netif, "esp-vcp"); // Set your desired hostname

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        wifi_config_t wifi_config = {};
        strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
        strcpy((char *)wifi_config.sta.password, WIFI_PASSWORD);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI("wifi", "Wi-Fi started, connecting to SSID: %s", wifi_config.sta.ssid);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            ESP_LOGE("wifi", "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }

        // Wait for connection and print IP
        ESP_LOGI("wifi", "Waiting for connection...");
        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, [](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
                                                            {
                                                            if (event_id == WIFI_EVENT_STA_CONNECTED) {
                                                                ESP_LOGI("wifi", "Connected to AP");
                                                            } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
                                                                ESP_LOGW("wifi", "Disconnected from AP");
                                                            } }, NULL, &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
                                                            {
                                                            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                                                            ESP_LOGI("wifi", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip)); }, NULL, &instance_got_ip));

        // Wait until got IP
        while (true)
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
            {
                ESP_LOGI("wifi", "Connected with IP: " IPSTR, IP2STR(&ip_info.ip));
                break;
            }
        }
    }

    esp_err_t ota_update_handler(httpd_req_t *req)
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

    esp_err_t upload_page_handler(httpd_req_t *req)
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

    esp_err_t firmware_upload_handler(httpd_req_t *req)
    {
        // Allocate buffer for firmware (adjust size as needed)
        const size_t max_fw_size = 2 * 1024 * 1024; // 2MB
        uint8_t *fw_buf = (uint8_t *)malloc(max_fw_size);
        if (!fw_buf)
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
            return ESP_FAIL;
        }

        int received = 0;
        int remaining = req->content_len;
        while (remaining > 0 && received < max_fw_size)
        {
            int to_read = remaining > 4096 ? 4096 : remaining;
            int r = httpd_req_recv(req, (char *)fw_buf + received, to_read);
            if (r <= 0)
            {
                free(fw_buf);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
                return ESP_FAIL;
            }
            received += r;
            remaining -= r;
        }

        // Start OTA update from buffer
        esp_err_t ret = ESP_OK;
        esp_ota_handle_t ota_handle = 0;
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition)
        {
            free(fw_buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
            return ESP_FAIL;
        }
        ret = esp_ota_begin(update_partition, received, &ota_handle);
        if (ret != ESP_OK)
        {
            free(fw_buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            return ESP_FAIL;
        }
        ret = esp_ota_write(ota_handle, fw_buf, received);
        free(fw_buf);
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
}

/**
 * @brief Main application
 *
 * This function shows how you can use Virtual COM Port drivers
 */
extern "C" void app_main(void)
{
    // Mount LittleFS
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .partition = NULL,
        .format_if_mount_failed = false,
        .read_only = true,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        return;
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    wifi_init_sta(); // Add this line

    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    serial_buffer_mutex = xSemaphoreCreateMutex();
    assert(serial_buffer_mutex);
    serial_buffer_len = 0;

    // Start web server
    start_webserver();

    // Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    assert(task_created == pdTRUE);

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    // Register VCP drivers to VCP service
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();

    // Do everything else in a loop, so we can demonstrate USB device reconnections
    while (true)
    {
        const cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 5000,
            .out_buffer_size = 64,
            .in_buffer_size = 64,
            .event_cb = handle_event,
            .data_cb = handle_rx,
            .user_arg = NULL,
        };

        ESP_LOGI(TAG, "Opening any VCP device...");
        vcp = std::unique_ptr<CdcAcmDevice>(VCP::open(&dev_config));

        if (vcp == nullptr)
        {
            ESP_LOGI(TAG, "Failed to open VCP device, retrying...");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Setting up line coding");
        cdc_acm_line_coding_t line_coding = {
            .dwDTERate = BAUDRATE,
            .bCharFormat = STOP_BITS,
            .bParityType = PARITY,
            .bDataBits = DATA_BITS,
        };
        ESP_ERROR_CHECK(vcp->line_coding_set(&line_coding));
        ESP_ERROR_CHECK(vcp->set_control_line_state(true, true));

        ESP_LOGI(TAG, "VCP device connected. Waiting for disconnection...");
        // Stay connected and process data until device is disconnected
        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);

        ESP_LOGI(TAG, "VCP device disconnected. Cleaning up...");
        vcp.reset();
        // Optionally clear serial buffer here if desired
        serial_buffer_len = 0;
    }
}
