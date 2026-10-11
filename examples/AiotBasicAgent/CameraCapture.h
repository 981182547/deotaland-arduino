#pragma once

#include <stddef.h>
#include <stdint.h>
#include <agent_link_platform.h>

#if defined(ARDUINO_ARCH_ESP32) && __has_include("esp_camera.h")
#include "sdkconfig.h"
// Camera SCCB explicitly uses I2C0. The new SCCB backend in the tested Core
// 3.3.12 can reuse this bus; the user's PDM mic/I2S amplifier needs no I2C.
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW
#define DEOTALAND_CAMERA_AVAILABLE 1
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#else
#define DEOTALAND_CAMERA_AVAILABLE 0
#endif
#else
#define DEOTALAND_CAMERA_AVAILABLE 0
#endif

// A result owns its JPEG until release(). No camera-driver framebuffer leaves
// the capture worker. A connection epoch lets the sketch discard stale photos.
struct CameraImage {
  uint8_t* data = nullptr;
  size_t length = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t epoch = 0;
  esp_err_t result = ESP_OK;
};

class CameraCapture {
public:
  CameraCapture() = default;
  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;
  esp_err_t begin();
  bool request(uint32_t connectionEpoch);
  bool take(CameraImage& image);
  bool busy() const { return busy_; }
  static void release(CameraImage& image);

private:
  bool busy_ = false;
#if DEOTALAND_CAMERA_AVAILABLE
  QueueHandle_t requests_ = nullptr;
  QueueHandle_t results_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  void* sccbBus_ = nullptr;
  bool gc2145_ = false;
  static void workerEntry(void* context);
  void workerLoop();
  CameraImage capture(uint32_t epoch);
#endif
};
