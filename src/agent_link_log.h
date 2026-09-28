#pragma once
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_log.h>
#else
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#endif
