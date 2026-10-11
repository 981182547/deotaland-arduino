#pragma once

// Keep Arduino's recursive library build portable. Unsupported chips, host
// tests, and external BLE backends must not compile the embedded host stack.
#if defined(ARDUINO_ARCH_ESP32) && defined(ESP_PLATFORM) && \
    !defined(AGENT_LINK_DISABLE_BLE) && !defined(DEOTALAND_USE_IDF_NIMBLE)
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if defined(SOC_BLE_SUPPORTED) && SOC_BLE_SUPPORTED
#define DEOTALAND_NIMBLE_ACTIVE 1
#endif
#endif

#ifndef DEOTALAND_NIMBLE_ACTIVE
#define DEOTALAND_NIMBLE_ACTIVE 0
#endif
