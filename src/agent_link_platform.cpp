#include "agent_link_platform.h"
#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#endif

uint32_t agent_link_now_ms(void) {
#if defined(ARDUINO)
    return static_cast<uint32_t>(millis());
#else
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

uint32_t agent_link_now_us(void) {
#if defined(ARDUINO)
    return static_cast<uint32_t>(micros());
#else
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

#if !defined(ARDUINO_ARCH_ESP32)
const char* esp_err_to_name(esp_err_t e) {
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_FAIL";
    }
}
#endif
