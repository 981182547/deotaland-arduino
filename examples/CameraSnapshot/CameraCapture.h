#pragma once

#include <stddef.h>
#include <stdint.h>
#include <agent_link_platform.h>

#if defined(ARDUINO_ARCH_ESP32) && __has_include("esp_camera.h") && __has_include("img_converters.h")
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
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

// A result owns its malloc-allocated JPEG until release(). No driver framebuffer
// crosses threads, and the worker never calls any Deotaland API.
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
  esp_err_t begin(); // Camera initialization happens once in setup().
  bool request(uint32_t connectionEpoch); // Nonblocking, loop-thread only.
  bool take(CameraImage& image); // Transfers ownership to the loop thread.
  bool busy() const { return busy_; }
  static void release(CameraImage& image);

private:
  bool busy_ = false;
#if DEOTALAND_CAMERA_AVAILABLE
  QueueHandle_t requests_ = nullptr;
  QueueHandle_t results_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  bool gc2145_ = false;
  static void workerEntry(void* context);
  void workerLoop();
  CameraImage capture(uint32_t epoch);
#endif
};
