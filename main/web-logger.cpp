#include "web-logger.h"
#include "http-server.h"
#include <cstdarg>
#include <cstdio>
#include <esp_log.h>

std::shared_ptr<HttpServer> WebLogger::httpServer = nullptr;

int WebLogger::vprintf_handler(const char *fmt, va_list args) {
  // Format the log message in a temporary buffer
  static char buffer[512];
  int len = vsnprintf(buffer, sizeof(buffer), fmt, args);

  if (len > 0 && httpServer) {
    // Send to web clients
    httpServer->log_to_web((const uint8_t *)buffer, len);
  }

  // Also send to UART (standard output) so serial monitor still works
  return vfprintf(stdout, fmt, args);
}

void WebLogger::init(std::shared_ptr<HttpServer> server) {
  httpServer = server;
  // Register our custom vprintf handler to capture all ESP_LOG* output
  esp_log_set_vprintf(vprintf_handler);
  ESP_LOGI("WebLogger", "Web logging initialized");
}
