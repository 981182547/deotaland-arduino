#pragma once
// Internal build gate shared by the backend, portal and portable stubs.
#if defined(ARDUINO_ARCH_ESP32) && !defined(AGENT_LINK_DISABLE_WIFI)
#include "soc/soc_caps.h"
#if defined(SOC_WIFI_SUPPORTED) && SOC_WIFI_SUPPORTED
#define AGENT_LINK_WIFI_AVAILABLE 1
#endif
#endif
#ifndef AGENT_LINK_WIFI_AVAILABLE
#define AGENT_LINK_WIFI_AVAILABLE 0
#endif
#if AGENT_LINK_WIFI_AVAILABLE && !defined(AGENT_LINK_DISABLE_CLOUD)
#define AGENT_LINK_CLOUD_AVAILABLE 1
#else
#define AGENT_LINK_CLOUD_AVAILABLE 0
#endif
