#pragma once

#include <stddef.h>
#include <stdint.h>
#include <agent_link_platform.h>

// AIOT-Basic ST7789 display. Keep this object alive until device reset.
// Call the public methods from loop()/Deotaland callbacks on that same thread.
// Text setters only copy bounded strings; decoding/SPI run on a private task.
class BoardDisplay {
public:
  BoardDisplay() = default;
  BoardDisplay(const BoardDisplay&) = delete;
  BoardDisplay& operator=(const BoardDisplay&) = delete;

  esp_err_t begin();
  bool ready() const;
  void showStatus(const char* title, const char* hint);
  void showText(const char* utf8);
  void poll();

  // Loop-thread only, not an ISR or a text callback. Copies at most 128 KiB into
  // PSRAM; the caller may release its original JPEG as soon as this returns.
  // Actual decoding/drawing is asynchronous. Baseline images up to VGA only.
  esp_err_t showJpeg(const uint8_t* data, size_t length);
  esp_err_t lastError() const;

private:
  struct Impl;
  Impl* impl_ = nullptr;
};
