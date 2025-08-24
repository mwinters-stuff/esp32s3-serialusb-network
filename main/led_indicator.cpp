#include "led_indicator.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <cmath>

static const char *TAG = "LED";

LedIndicator::LedIndicator() : currentState(LedState::IDLE), strip_handle(NULL) {
    stateMutex = xSemaphoreCreateMutex();
}

void LedIndicator::init() {
    ESP_LOGI(TAG, "Initializing addressable LED on GPIO %d", LED_PIN);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_PIN,
        .max_leds = 1, // The number of LEDs in the strip
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_handle));

    if (!strip_handle) {
        ESP_LOGE(TAG, "Install of RMT-based LED driver failed");
        return;
    }

    // Clear LED strip (turn off)
    ESP_ERROR_CHECK(led_strip_clear(strip_handle));

    xTaskCreate(led_task, "led_task", 2048, this, 5, NULL);
    ESP_LOGI(TAG, "LED indicator initialized.");
}

void LedIndicator::setState(LedState newState) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    // The ERROR state is latched and can't be changed until reboot.
    if (currentState == LedState::ERROR) {
        xSemaphoreGive(stateMutex);
        return;
    }
    // The UPLOADING state is high-priority and should only be superseded by an ERROR.
    if (currentState == LedState::UPLOADING && newState != LedState::ERROR) {
        xSemaphoreGive(stateMutex);
        return;
    }

    currentState = newState;
    xSemaphoreGive(stateMutex);
}

LedState LedIndicator::getState() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    LedState localState = currentState;
    xSemaphoreGive(stateMutex);
    return localState;
}

void LedIndicator::setColor(uint32_t r, uint32_t g, uint32_t b) {
    if (strip_handle) {
        // The led_strip_set_pixel function is 0-indexed.
        ESP_ERROR_CHECK(led_strip_set_pixel(strip_handle, 0, r, g, b));
        ESP_ERROR_CHECK(led_strip_refresh(strip_handle));
    }
}

void LedIndicator::pulse(uint32_t r, uint32_t g, uint32_t b) {
    float angle = (float)esp_log_timestamp() / 400.0f;
    float brightness = (sin(angle) + 1.0f) / 2.0f;
    brightness = 0.1f + brightness * 0.9f; // Pulse from 10% to 100%
    setColor((uint32_t)(r * brightness), (uint32_t)(g * brightness), (uint32_t)(b * brightness));
}

void LedIndicator::led_task(void *arg) {
    static_cast<LedIndicator*>(arg)->run();
}

void LedIndicator::run() {
    while (1) {
        LedState localState = getState();

        switch (localState) {
            case LedState::IDLE:                pulse(0, 0, 255); break; // Pulsing Blue
            case LedState::WIFI_DISCONNECTED:   pulse(255, 165, 0); break; // Pulsing Orange
            case LedState::USB_CONNECTED:       setColor(0, 255, 0); break; // Solid Green
            case LedState::WEB_TERMINAL_ACTIVE: setColor(0, 255, 255); break; // Solid Cyan
            case LedState::UPLOADING:           pulse(255, 0, 255); break; // Pulsing Magenta
            case LedState::ERROR:               setColor(255, 0, 0); break; // Solid Red
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}