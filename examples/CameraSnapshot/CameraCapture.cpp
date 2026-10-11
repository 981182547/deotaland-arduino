#include "CameraCapture.h"

#include <stdlib.h>
#include <string.h>

#if DEOTALAND_CAMERA_AVAILABLE
#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "camera_pins.h"

esp_err_t CameraCapture::begin() {
  if (worker_) return ESP_ERR_INVALID_STATE;
#if CAMERA_REQUIRE_PSRAM
  if (!psramFound()) return ESP_ERR_NO_MEM;
#endif
  requests_ = xQueueCreate(1, sizeof(uint32_t));
  results_ = xQueueCreate(1, sizeof(CameraImage));
  if (!requests_ || !results_) {
    if (requests_) vQueueDelete(requests_);
    if (results_) vQueueDelete(results_);
    requests_ = results_ = nullptr;
    return ESP_ERR_NO_MEM;
  }

  camera_config_t config = {};
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_pclk = CAM_PIN_PCLK;
  config.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = CAMERA_PIXEL_FORMAT;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.jpeg_quality = CAMERA_SENSOR_JPEG_QUALITY;
  config.fb_count = CAMERA_FB_COUNT;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t result = esp_camera_init(&config);
  if (result == ESP_OK) {
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
      gc2145_ = sensor->id.PID == GC2145_PID;
      if (sensor->set_vflip) sensor->set_vflip(sensor, CAMERA_VFLIP);
      if (sensor->set_hmirror) sensor->set_hmirror(sensor, CAMERA_HMIRROR);
      // GC2145 powers up with its capture engine held in reset. Pulse only
      // this sensor's CISCTL_restart_n; these registers differ on other sensors.
      if (gc2145_ && sensor->set_reg) {
        if (sensor->set_reg(sensor, 0xfe, 0xff, 0x00) != 0) result = ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (sensor->set_reg(sensor, 0xfe, 0xff, 0x10) != 0) result = ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
    if (result == ESP_OK &&
        xTaskCreate(workerEntry, "camera_jpeg", 8192, this, 1, &worker_) != pdPASS)
      result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) esp_camera_deinit();
  }
  if (result != ESP_OK) {
    vQueueDelete(requests_);
    vQueueDelete(results_);
    requests_ = results_ = nullptr;
    worker_ = nullptr;
  }
  return result;
}

bool CameraCapture::request(uint32_t epoch) {
  if (!worker_ || busy_) return false;
  if (xQueueSend(requests_, &epoch, 0) != pdTRUE) return false;
  busy_ = true;
  return true;
}

bool CameraCapture::take(CameraImage& image) {
  if (!results_ || xQueueReceive(results_, &image, 0) != pdTRUE) return false;
  busy_ = false;
  return true;
}

void CameraCapture::workerEntry(void* context) {
  static_cast<CameraCapture*>(context)->workerLoop();
}

void CameraCapture::workerLoop() {
  for (;;) {
    uint32_t epoch = 0;
    if (xQueueReceive(requests_, &epoch, portMAX_DELAY) != pdTRUE) continue;
    CameraImage image = capture(epoch);
    // One request is allowed until the loop consumes its result. The queue
    // therefore has space; blocking here also preserves ownership if delayed.
    xQueueSend(results_, &image, portMAX_DELAY);
  }
}

CameraImage CameraCapture::capture(uint32_t epoch) {
  CameraImage image;
  image.epoch = epoch;
  // Discard buffers queued before the trigger, then capture a fresh frame.
  // All potentially blocking fb_get/encoding work stays in this worker.
  for (size_t i = 0; i < CAMERA_FB_COUNT; ++i) {
    camera_fb_t* old = esp_camera_fb_get();
    if (!old) { image.result = ESP_ERR_TIMEOUT; return image; }
    esp_camera_fb_return(old);
  }
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) { image.result = ESP_ERR_TIMEOUT; return image; }
  if (!frame->buf || !frame->len || !frame->width || !frame->height ||
      frame->width > UINT16_MAX || frame->height > UINT16_MAX || frame->len > UINT32_MAX) {
    image.result = ESP_ERR_INVALID_SIZE;
  } else {
    image.width = static_cast<uint16_t>(frame->width);
    image.height = static_cast<uint16_t>(frame->height);
    if (frame->format == PIXFORMAT_JPEG) {
      // Copy native JPEG so the driver can immediately reuse its framebuffer.
      image.data = static_cast<uint8_t*>(malloc(frame->len));
      if (!image.data) image.result = ESP_ERR_NO_MEM;
      else { memcpy(image.data, frame->buf, frame->len); image.length = frame->len; }
    } else if (frame->format == PIXFORMAT_RGB565) {
      const size_t rowBytes = frame->width * 2u;
      if (frame->height > SIZE_MAX / rowBytes || frame->len < rowBytes * frame->height) {
        image.result = ESP_ERR_INVALID_SIZE;
      } else {
        // The GC2145's first rows can flicker. Copy the first clean row over
        // them. Keep original RGB565 byte order for frame2jpg, as upstream does.
        const size_t maskRows = CAMERA_MASK_TOP_ROWS;
        if (gc2145_ && maskRows > 0 && maskRows < frame->height) {
          const uint8_t* clean = frame->buf + maskRows * rowBytes;
          for (size_t row = 0; row < maskRows; ++row)
            memcpy(frame->buf + row * rowBytes, clean, rowBytes);
        }
        if (!frame2jpg(frame, CAMERA_JPEG_QUALITY, &image.data, &image.length))
          image.result = ESP_FAIL;
      }
    } else image.result = ESP_ERR_NOT_SUPPORTED;
  }
  esp_camera_fb_return(frame); // Always returned, including encode/allocate failures.
  if (image.result == ESP_OK && (!image.data || !image.length || image.length > UINT32_MAX))
    image.result = ESP_ERR_INVALID_SIZE;
  if (image.result != ESP_OK) { free(image.data); image.data = nullptr; image.length = 0; }
  return image;
}

#else
esp_err_t CameraCapture::begin() { return ESP_ERR_NOT_SUPPORTED; }
bool CameraCapture::request(uint32_t) { return false; }
bool CameraCapture::take(CameraImage&) { return false; }
#endif

void CameraCapture::release(CameraImage& image) {
  free(image.data);
  image = CameraImage{};
}
