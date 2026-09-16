#include "arduino-ota.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/md5.h>

#include "config.h"

static const char *TAG = "ESPOTA";

// espota.py protocol constants
static constexpr int OTA_CMD_FLASH = 0;
static constexpr int OTA_CMD_SPIFFS = 100;
static constexpr int OTA_CMD_AUTH = 200;

static constexpr size_t OTA_CHUNK_SIZE = 1460;

static std::shared_ptr<LedIndicator> s_led;

static void md5_hex(const uint8_t *data, size_t len, char out[33])
{
  uint8_t digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, data, len);
  mbedtls_md5_finish(&ctx, digest);
  mbedtls_md5_free(&ctx);
  for (int i = 0; i < 16; i++)
  {
    sprintf(out + (i * 2), "%02x", digest[i]);
  }
  out[32] = '\0';
}

static void set_recv_timeout(int sock, int ms)
{
  struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void udp_reply(int sock, const struct sockaddr_in &to, const char *msg)
{
  sendto(sock, msg, strlen(msg), 0, (const struct sockaddr *)&to, sizeof(to));
}

static bool authenticate(int sock, const struct sockaddr_in &from)
{
  uint8_t rnd[16];
  esp_fill_random(rnd, sizeof(rnd));
  char nonce[33];
  md5_hex(rnd, sizeof(rnd), nonce);

  char msg[64];
  snprintf(msg, sizeof(msg), "AUTH %s", nonce);
  udp_reply(sock, from, msg);

  char buf[192];
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);
  set_recv_timeout(sock, 5000);
  int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
  set_recv_timeout(sock, 0);
  if (len <= 0)
  {
    ESP_LOGW(TAG, "No auth response");
    return false;
  }
  buf[len] = '\0';

  int cmd = 0;
  char cnonce[64] = {0};
  char response[64] = {0};
  if (sscanf(buf, "%d %63s %63s", &cmd, cnonce, response) != 3 || cmd != OTA_CMD_AUTH)
  {
    udp_reply(sock, from, "Authentication Failed");
    return false;
  }

  char pass_md5[33];
  md5_hex((const uint8_t *)OTA_PASSWORD, strlen(OTA_PASSWORD), pass_md5);

  char challenge[160];
  int challenge_len = snprintf(challenge, sizeof(challenge), "%s:%s:%s", pass_md5, nonce, cnonce);

  char expected[33];
  md5_hex((const uint8_t *)challenge, challenge_len, expected);

  if (strcasecmp(expected, response) != 0)
  {
    ESP_LOGW(TAG, "Authentication failed");
    udp_reply(sock, from, "Authentication Failed");
    return false;
  }
  return true;
}

static void run_update(const struct sockaddr_in &remote, uint16_t host_port, size_t size,
                       const char *expected_md5)
{
  const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
  if (!update)
  {
    ESP_LOGE(TAG, "No OTA partition available");
    return;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0)
  {
    ESP_LOGE(TAG, "socket() failed: %d", errno);
    return;
  }

  struct sockaddr_in dest = remote;
  dest.sin_port = htons(host_port);
  if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0)
  {
    ESP_LOGE(TAG, "Connect back to host failed: %d", errno);
    close(sock);
    return;
  }
  set_recv_timeout(sock, 10000);

  if (s_led) s_led->setState(LedState::UPLOADING);

  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(update, size, &ota);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    send(sock, "ERR: OTA begin failed\n", 22, 0);
    close(sock);
    if (s_led) s_led->setState(LedState::ERROR);
    return;
  }

  uint8_t *buf = (uint8_t *)malloc(OTA_CHUNK_SIZE);
  if (!buf)
  {
    esp_ota_abort(ota);
    send(sock, "ERR: No memory\n", 15, 0);
    close(sock);
    if (s_led) s_led->setState(LedState::ERROR);
    return;
  }

  mbedtls_md5_context md5;
  mbedtls_md5_init(&md5);
  mbedtls_md5_starts(&md5);

  ESP_LOGI(TAG, "Receiving %u bytes into partition %s at 0x%08" PRIx32,
           (unsigned)size, update->label, update->address);

  size_t received = 0;
  bool ok = true;
  while (received < size)
  {
    const size_t want = (size - received) < OTA_CHUNK_SIZE ? (size - received) : OTA_CHUNK_SIZE;
    int r = recv(sock, buf, want, 0);
    if (r <= 0)
    {
      ESP_LOGE(TAG, "recv failed after %u bytes (errno %d)", (unsigned)received, errno);
      ok = false;
      break;
    }

    if (esp_ota_write(ota, buf, r) != ESP_OK)
    {
      ESP_LOGE(TAG, "esp_ota_write failed at %u bytes", (unsigned)received);
      send(sock, "ERR: Flash write failed\n", 24, 0);
      ok = false;
      break;
    }

    mbedtls_md5_update(&md5, buf, r);
    received += r;

    char ack[16];
    int ack_len = snprintf(ack, sizeof(ack), "%d", r);
    if (send(sock, ack, ack_len, 0) < 0)
    {
      ok = false;
      break;
    }
  }

  free(buf);

  uint8_t digest[16];
  mbedtls_md5_finish(&md5, digest);
  mbedtls_md5_free(&md5);

  if (ok && expected_md5[0] != '\0')
  {
    char actual[33];
    for (int i = 0; i < 16; i++)
    {
      sprintf(actual + (i * 2), "%02x", digest[i]);
    }
    actual[32] = '\0';
    if (strcasecmp(actual, expected_md5) != 0)
    {
      ESP_LOGE(TAG, "MD5 mismatch: got %s expected %s", actual, expected_md5);
      send(sock, "ERR: MD5 mismatch\n", 18, 0);
      ok = false;
    }
  }

  if (!ok)
  {
    esp_ota_abort(ota);
    close(sock);
    if (s_led) s_led->setState(LedState::ERROR);
    return;
  }

  err = esp_ota_end(ota);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    send(sock, "ERR: Image validation failed\n", 29, 0);
    close(sock);
    if (s_led) s_led->setState(LedState::ERROR);
    return;
  }

  err = esp_ota_set_boot_partition(update);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    send(sock, "ERR: Set boot partition failed\n", 31, 0);
    close(sock);
    if (s_led) s_led->setState(LedState::ERROR);
    return;
  }

  send(sock, "OK", 2, 0);
  close(sock);

  ESP_LOGI(TAG, "OTA complete (%u bytes). Rebooting...", (unsigned)received);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
}

static void arduino_ota_task(void *arg)
{
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
  {
    ESP_LOGE(TAG, "Failed to create UDP socket: %d", errno);
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in bind_addr = {};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(ARDUINO_OTA_PORT);
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0)
  {
    ESP_LOGE(TAG, "Failed to bind UDP port %d: %d", ARDUINO_OTA_PORT, errno);
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "ArduinoOTA (espota) listening on UDP port %d", ARDUINO_OTA_PORT);

  while (true)
  {
    char buf[256];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
    if (len <= 0)
    {
      continue;
    }
    buf[len] = '\0';

    int cmd = 0;
    int host_port = 0;
    unsigned int size = 0;
    char md5[64] = {0};
    if (sscanf(buf, "%d %d %u %63s", &cmd, &host_port, &size, md5) < 3)
    {
      ESP_LOGW(TAG, "Malformed invitation: %s", buf);
      continue;
    }

    if (cmd == OTA_CMD_SPIFFS)
    {
      ESP_LOGW(TAG, "Filesystem upload over espota is not supported; use the web upload page");
      udp_reply(sock, from, "ERR: Filesystem OTA not supported");
      continue;
    }

    if (cmd != OTA_CMD_FLASH)
    {
      continue;
    }

    if (size == 0 || size > 0x280000)
    {
      udp_reply(sock, from, "ERR: Bad image size");
      continue;
    }

    ESP_LOGI(TAG, "OTA invitation from %s:%d, %u bytes",
             inet_ntoa(from.sin_addr), host_port, size);

    if (strlen(OTA_PASSWORD) > 0 && !authenticate(sock, from))
    {
      continue;
    }

    udp_reply(sock, from, "OK");
    run_update(from, (uint16_t)host_port, size, md5);
  }
}

void arduino_ota_start(std::shared_ptr<LedIndicator> led)
{
  s_led = led;
  xTaskCreate(arduino_ota_task, "arduino_ota", 6144, NULL, 5, NULL);
}
