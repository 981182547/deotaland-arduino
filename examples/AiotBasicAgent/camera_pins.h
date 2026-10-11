#pragma once

// AIOT-Basic DVP wiring; supports OV2640 and original-project GC2145 sensors.
// These are not the AI-Thinker ESP32-CAM pins. Camera owns SCCB I2C0 on GPIO4/5.
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
#define CAMERA_SCCB_I2C_PORT   0
#define CAPTURE_BUTTON_PIN    -1 // GPIO3 is SD SCLK on AIOT-Basic, not a button.
#define CAMERA_BUTTON_DEBOUNCE_MS 30
// Default RGB565 works with both sensor families, then frame2jpg produces JPEG.
// Only set this to 1 after confirming OV2640/native-JPEG sensor hardware.
#define CAMERA_NATIVE_JPEG     0
#if CAMERA_NATIVE_JPEG
#define CAMERA_PIXEL_FORMAT   PIXFORMAT_JPEG
#else
#define CAMERA_PIXEL_FORMAT   PIXFORMAT_RGB565
#endif
#define CAMERA_FRAME_SIZE     FRAMESIZE_240X240
#define CAMERA_FB_COUNT       2
#define CAMERA_JPEG_QUALITY   80 // Software encoder: 1..100, higher is better.
#define CAMERA_SENSOR_JPEG_QUALITY 12 // Native JPEG: 0..63, lower is better.
#define CAMERA_MASK_TOP_ROWS   8 // Only applied to GC2145 RGB565 frames.
#define CAMERA_VFLIP           0
#define CAMERA_HMIRROR         0
#define CAMERA_REQUIRE_PSRAM   1
#define CAMERA_CAPTURE_TIMEOUT_MS 15000u
#define CAMERA_UPLOAD_TIMEOUT_MS  30000u

// The complete sketch may schedule local previews independently. These text
// prompts are disabled; a prompt does not itself invoke camera0 or attach JPEG.
#define CAMERA_AUTO_PROMPT_INTERVAL_MS 0u
#define CAMERA_AUTO_PROMPT_TEXT "拍一张照片，然后分析照片中有没有人"
