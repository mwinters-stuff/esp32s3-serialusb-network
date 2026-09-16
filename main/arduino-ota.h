#ifndef _ARDUINO_OTA_H
#define _ARDUINO_OTA_H

#include <memory>

#include "led_indicator.h"

// Starts the ArduinoOTA (espota.py) listener task.
// Works over any active interface (W5500 ethernet or WiFi) since it uses plain sockets.
void arduino_ota_start(std::shared_ptr<LedIndicator> led);

#endif
