#ifndef _WEB_LOGGER_H
#define _WEB_LOGGER_H

#include <cstddef>
#include <memory>

class HttpServer; // Forward declaration

/**
 * WebLogger captures ESP_LOG* messages and sends them to connected WebSocket
 * clients. Call web_logger_init() after creating the HttpServer instance to
 * enable logging.
 */
class WebLogger {
private:
  static std::shared_ptr<HttpServer> httpServer;
  static int vprintf_handler(const char *fmt, va_list args);

public:
  /**
   * Initialize the web logger with an HttpServer instance.
   * This must be called after HttpServer is created and before logging is
   * needed.
   */
  static void init(std::shared_ptr<HttpServer> server);
};

#endif // _WEB_LOGGER_H
