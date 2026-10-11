#pragma once

// Default wiring: Agent_link-main GC2145 camera board, ESP32-S3 with PSRAM.
// Change these values to match your board before uploading the sketch.
// -1 means not connected. These pins are not the ESP32-CAM/OV2640 pin map.
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK   13

#define CAM_XCLK_FREQ_HZ       20000000
#define CAPTURE_BUTTON_PIN     3  // Active HIGH, internal pull-down; -1 disables.
#define CAMERA_BUTTON_DEBOUNCE_MS 30

// GC2145 has no native JPEG output. Keep RGB565 and encode with frame2jpg.
// For a sensor with native JPEG (e.g. OV2640), select PIXFORMAT_JPEG and its pins.
#define CAMERA_PIXEL_FORMAT   PIXFORMAT_RGB565
#define CAMERA_FRAME_SIZE     FRAMESIZE_240X240
#define CAMERA_FB_COUNT       2
#define CAMERA_JPEG_QUALITY   80  // Software encoder: 1..100, higher is better.
#define CAMERA_SENSOR_JPEG_QUALITY 12  // Native JPEG: 0..63, lower is better.
#define CAMERA_MASK_TOP_ROWS   8  // Applied only to GC2145 RGB565 frames.
#define CAMERA_VFLIP           0
#define CAMERA_HMIRROR         0
#define CAMERA_REQUIRE_PSRAM   1  // Default double RGB565 buffers need PSRAM.
#define CAMERA_CAPTURE_TIMEOUT_MS 15000u
#define CAMERA_UPLOAD_TIMEOUT_MS  30000u

// Optional original-board text prompt. 0 disables automatic prompts.
// Set to 10000u for the original 10-second cadence. This sends text only;
// the Agent must invoke camera0=true to request a new picture.
#define CAMERA_AUTO_PROMPT_INTERVAL_MS 0u
#define CAMERA_AUTO_PROMPT_TEXT "拍一张照片，然后分析照片中有没有人"
