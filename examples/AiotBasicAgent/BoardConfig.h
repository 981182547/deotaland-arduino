#pragma once

// Defaults match the user's AIOT-Basic wiring and https://arduino.me/a/3218.
// This is the independent I2S amplifier + PDM microphone version, not the
// ES8311 variant described by newer oj_esp32s3basic/board.json files.
// Camera wiring is in camera_pins.h; audio/LCD wiring is at the top of their
// BoardAudio.cpp / BoardDisplay.cpp. Do not substitute older Basic kit pins.
#define AIOT_DEVICE_NAME "Deotaland-AIOT-Basic"
#define AIOT_HARDWARE_MODEL "OJ-AIOT-BASIC"
#define AIOT_FIRMWARE_VERSION "1.0.0"

// BLE is the complete control/media path. WiFi implements the upstream
// provisioning/activation/heartbeat path only; it has no Agent media session.
#ifndef AIOT_USE_WIFI
#define AIOT_USE_WIFI 0
#endif
#ifndef AIOT_ENABLE_OTA
#define AIOT_ENABLE_OTA 1
#endif
#define AIOT_WIFI_SSID ""
#define AIOT_WIFI_PASSWORD ""
// Leave product ID zero for network-only provisioning. For cloud activation,
// supply your own HTTPS origin (no trailing slash), product ID and chip type.
#define AIOT_PLATFORM_ORIGIN ""
#define AIOT_PLATFORM_PRODUCT_ID 0
#define AIOT_PLATFORM_CHIP_TYPE "esp32s3"

#define AIOT_RGB_LED_PIN 48
// Board JSON does not specify a physical PTT button. Serial p/r works as-is.
// An optional wired button uses INPUT_PULLUP and is active LOW. Check that its
// GPIO does not overlap camera, audio, LCD, USB or SD pins before enabling it.
#define AIOT_PTT_BUTTON_PIN (-1)
#define AIOT_BUTTON_DEBOUNCE_MS 25u
#define AIOT_CAPTURE_LIMIT_MS 60000u
#define AIOT_IMAGE_TIMEOUT_MS 30000u
#define AIOT_CAMERA_TIMEOUT_MS 5000u
// Local preview is independent of IMAGE upload. Zero disables auto-preview.
#define AIOT_PREVIEW_INTERVAL_MS 1000u
#define AIOT_INITIAL_VOLUME 60u

// SD is wired SCLK3 / MISO35 / MOSI14 / CS46, on its own SPI bus.
#define AIOT_ENABLE_SD 1
// Storage format only: both PCM and IMA-ADPCM WAV are decoded to PCM16 for
// local playback/AUDIO upload. Default PCM preserves existing recordings.
#ifndef AIOT_SD_ADPCM
#define AIOT_SD_ADPCM 0
#endif
