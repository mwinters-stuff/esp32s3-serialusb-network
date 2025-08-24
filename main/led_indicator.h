#ifndef _LED_INDICATOR_H
#define _LED_INDICATOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

// IMPORTANT: Configure your addressable LED GPIO here
// Common for ESP32-S3 devkits with on-board RGB LEDs is GPIO 48
#define LED_PIN 48

enum class LedState {
    IDLE,                   // System on, no USB connection (Pulsing Blue)
    WIFI_DISCONNECTED,      // WiFi is not connected (Pulsing Orange)
    USB_CONNECTED,          // USB connected, no web activity (Solid Green)
    WEB_TERMINAL_ACTIVE,    // Web terminal is being used (Solid Cyan)
    UPLOADING,              // Firmware/FS upload in progress (Pulsing Magenta)
    ERROR                   // An error state (Solid Red)
};

class LedIndicator {
public:
    LedIndicator();
    void init();
    void setState(LedState newState);
    LedState getState();

private:
    static void led_task(void *arg);
    void run();
    void setColor(uint32_t r, uint32_t g, uint32_t b);
    void pulse(uint32_t r, uint32_t g, uint32_t b);

    volatile LedState currentState;
    SemaphoreHandle_t stateMutex;
    led_strip_handle_t strip_handle;
};

#endif