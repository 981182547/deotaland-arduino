/* Korvo 板蓝牙媒体联调：语音上行、TTS 下行、摄像头图片上行，全部走本库的 BLE 通道。
 * 硬件：ESP32-S3-Korvo-2 V3（ES8311 扬声器、ES7210 麦克风、GC2145 摄像头，引脚见下方常量）。
 * Arduino IDE：ESP32S3 Dev Module；Flash 16MB；PSRAM 选 OPI PSRAM；分区选 APP 足够大的方案；
 * 串口 115200。用板上 USB-UART 口看日志时关闭 USB CDC On Boot。
 * VOICE 上行只需 App 订阅 0xFFA1；TTS 下行、AUDIO 上行、IMAGE 上行都走 L2CAP CoC，
 * 需要启用 CoC 的 Core（CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM >= 2），且 App 打开对应通道。
 *
 * 串口命令（发送单个字母）：
 *   v  开始/结束 VOICE 上行：麦克风 -> GATT 0xFFA1，最长 10 秒
 *   a  开始/结束 AUDIO 上行：麦克风 -> CoC PSM 0x0081，最长 30 秒（App 的开始/停止采集命令同样生效）
 *   c  拍一张 240x240 照片，JPEG 编码后 IMAGE 上行 -> CoC PSM 0x0082（App 控制 camera0 同样生效）
 *   t  本地扬声器自检：播放 1 秒 1 kHz 测试音，不经过蓝牙
 *   m  本地麦克风自检：统计 1 秒电平，不经过蓝牙
 *   ?  打印链路、各通道、播放和内存状态
 * TTS 下行无需命令：App 下发语音即自动播放，结束或断开时打印接收速率、欠载等统计。
 */
#include <Deotaland.h>
#include <Wire.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace {

// ── Korvo-2 V3 引脚 ──
constexpr int kI2cSda = 17;  // ES8311、ES7210、GC2145 共用这一条 I2C
constexpr int kI2cScl = 18;
constexpr gpio_num_t kI2sMclk = GPIO_NUM_16;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_9;
constexpr gpio_num_t kI2sWs = GPIO_NUM_45;
constexpr gpio_num_t kI2sDout = GPIO_NUM_8;   // ESP -> ES8311（扬声器）
constexpr gpio_num_t kI2sDin = GPIO_NUM_10;   // ES7210 -> ESP（麦克风）
constexpr int kPaEnablePin = 48;              // NS4150B 功放使能，高电平打开
constexpr uint8_t kEs8311Addr = 0x18;
constexpr uint8_t kEs7210Addr = 0x40;

constexpr int kCamXclk = 40, kCamPclk = 11, kCamVsync = 21, kCamHref = 38;
constexpr int kCamD0 = 13, kCamD1 = 47, kCamD2 = 14, kCamD3 = 3;
constexpr int kCamD4 = 12, kCamD5 = 42, kCamD6 = 41, kCamD7 = 39;
constexpr int kCamMaskTopRows = 8;  // GC2145 前几行是闪烁的黑白条，用下方第一行干净像素覆盖

// ── 音频参数：平台要求 PCM16、单声道、16 kHz，每毫秒 32 字节 ──
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kFrameSamples = 320;  // 20 ms
constexpr size_t kFrameBytes = kFrameSamples * sizeof(int16_t);
constexpr size_t kPlayBufBytes = 64 * 1024;  // TTS 缓冲 2 s，放 PSRAM
constexpr size_t kMicBufBytes = 16 * 1024;   // 麦克风缓冲 512 ms，吸收拍照编码时的 loop 停顿
constexpr uint8_t kDacVolume = 0xB3;         // ES8311 数字音量：0xBF = 0 dB，每步 0.5 dB
constexpr uint8_t kMicGain = 10;             // ES7210 PGA 档位：10 = 30 dB
constexpr uint32_t kVoiceMaxMs = 10000;
constexpr uint32_t kAudioMaxMs = 30000;
constexpr uint32_t kMicTestMs = 1000;
constexpr uint8_t kJpegQuality = 80;
constexpr size_t kImageChunkBytes = 512;

bool retryable(esp_err_t result) { return Deotaland.isRetryable(result); }

// ============================================================================
// 板级驱动：I2C 寄存器、ES8311/ES7210、I2S、GC2145
// ============================================================================

int i2cErrors = 0;

bool probe(uint8_t addr) {
  for (int attempt = 0; attempt < 3; ++attempt) {  // ES8311 上电后第一次访问偶尔 NACK
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return true;
    delay(5);
  }
  return false;
}

void writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) ++i2cErrors;
}

uint8_t readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(static_cast<uint16_t>(addr), static_cast<size_t>(1), true) != 1) {
    ++i2cErrors;
    return 0;
  }
  return static_cast<uint8_t>(Wire.read());
}

void updateReg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
  writeReg(addr, reg, (readReg(addr, reg) & ~mask) | (value & mask));
}

// 寄存器顺序移植自乐鑫 esp_codec_dev 的 es8311.c：从模式、使用 MCLK 引脚、
// MCLK = 4.096 MHz、16 kHz、16 bit、标准 I2S，仅 DAC。
bool initEs8311() {
  if (!probe(kEs8311Addr)) return false;
  i2cErrors = 0;
  const uint8_t a = kEs8311Addr;
  writeReg(a, 0x0D, 0xFA);
  writeReg(a, 0x44, 0x08);
  writeReg(a, 0x01, 0x30);
  writeReg(a, 0x02, 0x00);
  writeReg(a, 0x03, 0x10);
  writeReg(a, 0x16, 0x24);
  writeReg(a, 0x04, 0x10);
  writeReg(a, 0x05, 0x00);
  writeReg(a, 0x0B, 0x00);
  writeReg(a, 0x0C, 0x00);
  writeReg(a, 0x10, 0x1F);
  writeReg(a, 0x11, 0x7F);
  writeReg(a, 0x00, 0x80);         // 从模式
  writeReg(a, 0x01, 0x3F);         // 时钟来自 MCLK 引脚
  updateReg(a, 0x06, 0x20, 0x00);  // BCLK 不反相
  writeReg(a, 0x13, 0x10);
  writeReg(a, 0x1B, 0x0A);
  writeReg(a, 0x1C, 0x6A);
  writeReg(a, 0x44, 0x58);
  // 16 bit + 标准 I2S
  writeReg(a, 0x09, (readReg(a, 0x09) | 0x0C) & 0xFC);
  writeReg(a, 0x0A, (readReg(a, 0x0A) | 0x0C) & 0xFC);
  // 分频系数：MCLK 4.096 MHz / 16 kHz
  writeReg(a, 0x02, readReg(a, 0x02) & 0x07);
  writeReg(a, 0x05, 0x00);
  writeReg(a, 0x03, (readReg(a, 0x03) & 0x80) | 0x10);
  writeReg(a, 0x04, (readReg(a, 0x04) & 0x80) | 0x20);
  writeReg(a, 0x07, readReg(a, 0x07) & 0xC0);
  writeReg(a, 0x08, 0xFF);
  writeReg(a, 0x06, (readReg(a, 0x06) & 0xE0) | 0x03);
  // 启动 DAC
  writeReg(a, 0x00, 0x80);
  writeReg(a, 0x01, 0x3F);
  writeReg(a, 0x09, readReg(a, 0x09) & 0xBF);
  writeReg(a, 0x0A, readReg(a, 0x0A) & 0xBF);
  writeReg(a, 0x17, 0xBF);
  writeReg(a, 0x0E, 0x02);
  writeReg(a, 0x12, 0x00);
  writeReg(a, 0x14, 0x1A);
  writeReg(a, 0x0D, 0x01);
  writeReg(a, 0x15, 0x40);
  writeReg(a, 0x37, 0x08);
  writeReg(a, 0x45, 0x00);
  writeReg(a, 0x32, kDacVolume);
  writeReg(a, 0x31, readReg(a, 0x31) & 0x9F);  // 取消静音
  return i2cErrors == 0;
}

void es7210SelectMics(uint8_t gain) {
  const uint8_t a = kEs7210Addr;
  for (uint8_t reg = 0x43; reg <= 0x46; ++reg) updateReg(a, reg, 0x10, 0x00);
  writeReg(a, 0x4B, 0xFF);
  writeReg(a, 0x4C, 0xFF);
  // 只开 MIC1、MIC2（不进 TDM）：标准 I2S 下 MIC1 在左声道，I2S 单声道读取即得 MIC1。
  updateReg(a, 0x01, 0x0B, 0x00);
  writeReg(a, 0x4B, 0x00);
  updateReg(a, 0x43, 0x10, 0x10);
  updateReg(a, 0x43, 0x0F, gain);
  updateReg(a, 0x44, 0x10, 0x10);
  updateReg(a, 0x44, 0x0F, gain);
  writeReg(a, 0x12, 0x00);
}

// 寄存器顺序移植自乐鑫 esp_codec_dev 的 es7210.c：从模式、MIC1+MIC2、16 bit、标准 I2S。
bool initEs7210() {
  if (!probe(kEs7210Addr)) return false;
  i2cErrors = 0;
  const uint8_t a = kEs7210Addr;
  writeReg(a, 0x00, 0xFF);
  writeReg(a, 0x00, 0x41);
  writeReg(a, 0x01, 0x3F);
  writeReg(a, 0x09, 0x30);
  writeReg(a, 0x0A, 0x30);
  writeReg(a, 0x23, 0x2A);
  writeReg(a, 0x22, 0x0A);
  writeReg(a, 0x20, 0x0A);
  writeReg(a, 0x21, 0x2A);
  updateReg(a, 0x08, 0x01, 0x00);  // 从模式
  writeReg(a, 0x40, 0x43);
  writeReg(a, 0x41, 0x70);
  writeReg(a, 0x42, 0x70);
  writeReg(a, 0x07, 0x20);
  writeReg(a, 0x02, 0xC1);
  es7210SelectMics(kMicGain);
  const uint8_t clockOff = readReg(a, 0x01);
  writeReg(a, 0x11, (readReg(a, 0x11) & 0x1F) | 0x60);  // 16 bit
  writeReg(a, 0x11, readReg(a, 0x11) & 0xFC);           // 标准 I2S
  writeReg(a, 0x01, clockOff);
  writeReg(a, 0x06, 0x00);
  writeReg(a, 0x40, 0x43);
  writeReg(a, 0x47, 0x08);
  writeReg(a, 0x48, 0x08);
  writeReg(a, 0x49, 0x08);
  writeReg(a, 0x4A, 0x08);
  es7210SelectMics(kMicGain);
  writeReg(a, 0x40, 0x43);
  writeReg(a, 0x00, 0x71);
  writeReg(a, 0x00, 0x41);
  updateReg(a, 0x14, 0x03, 0x00);  // 取消静音
  updateReg(a, 0x15, 0x03, 0x00);
  return i2cErrors == 0;
}

i2s_chan_handle_t txChan = nullptr;
i2s_chan_handle_t rxChan = nullptr;

// 全双工：TX 接 ES8311，RX 接 ES7210，共用 MCLK/BCLK/WS。先让时钟跑起来，再配置编解码器。
esp_err_t initI2s() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // 播放缓冲为空时输出静音，不重复旧数据
  esp_err_t result = i2s_new_channel(&chan, &txChan, &rxChan);
  if (result != ESP_OK) return result;
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);  // MCLK = 256 x 16 kHz
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  cfg.gpio_cfg.mclk = kI2sMclk;
  cfg.gpio_cfg.bclk = kI2sBclk;
  cfg.gpio_cfg.ws = kI2sWs;
  cfg.gpio_cfg.dout = kI2sDout;
  cfg.gpio_cfg.din = kI2sDin;
  if ((result = i2s_channel_init_std_mode(txChan, &cfg)) != ESP_OK) return result;
  if ((result = i2s_channel_init_std_mode(rxChan, &cfg)) != ESP_OK) return result;
  if ((result = i2s_channel_enable(txChan)) != ESP_OK) return result;
  return i2s_channel_enable(rxChan);
}

uint16_t cameraPid = 0;

// esp32-camera 挂到 Wire 已在 GPIO17/18 上建好的 I2C0 总线，与编解码器共用。
esp_err_t initCamera() {
  camera_config_t c = {};
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.pin_xclk = kCamXclk;
  c.pin_sccb_sda = -1;
  c.pin_sccb_scl = -1;
  c.sccb_i2c_port = 0;
  c.pin_d7 = kCamD7;
  c.pin_d6 = kCamD6;
  c.pin_d5 = kCamD5;
  c.pin_d4 = kCamD4;
  c.pin_d3 = kCamD3;
  c.pin_d2 = kCamD2;
  c.pin_d1 = kCamD1;
  c.pin_d0 = kCamD0;
  c.pin_vsync = kCamVsync;
  c.pin_href = kCamHref;
  c.pin_pclk = kCamPclk;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_RGB565;  // GC2145 不支持硬件 JPEG，上传前用 frame2jpg 编码
  c.frame_size = FRAMESIZE_240X240;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  const esp_err_t result = esp_camera_init(&c);
  if (result != ESP_OK) return result;
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) {
    cameraPid = sensor->id.PID;
    if (sensor->set_reg) {
      // GC2145 上电后采集引擎处于复位，拉一次 CISCTL_restart_n（0xFE bit4）才会出帧。
      sensor->set_reg(sensor, 0xFE, 0xFF, 0x00);
      delay(20);
      sensor->set_reg(sensor, 0xFE, 0xFF, 0x10);
      delay(20);
    }
  }
  return ESP_OK;
}

void maskTopRows(camera_fb_t* fb) {
  const size_t rowBytes = static_cast<size_t>(fb->width) * 2u;
  if (fb->len < rowBytes * (kCamMaskTopRows + 1)) return;
  const uint8_t* clean = fb->buf + rowBytes * kCamMaskTopRows;
  for (int row = 0; row < kCamMaskTopRows; ++row) memcpy(fb->buf + rowBytes * row, clean, rowBytes);
}

// ============================================================================
// 音频任务：扬声器和麦克风各一个任务，与 loop 之间只通过 StreamBuffer 交换 PCM。
// 库 API 全部在 loop 线程调用；任务只碰 I2S 和缓冲区。
// ============================================================================

StreamBufferHandle_t playBuf = nullptr;  // 写：loop（库的播放回调、测试音）；读：扬声器任务
StreamBufferHandle_t micBuf = nullptr;   // 写：麦克风任务；读：loop
StaticStreamBuffer_t playBufState;
StaticStreamBuffer_t micBufState;
std::atomic<bool> flushSpeaker{false};
std::atomic<bool> ttsStreaming{false};
std::atomic<bool> micCapture{false};
std::atomic<uint32_t> speakerUnderruns{0};
std::atomic<uint32_t> micOverflowBytes{0};

StreamBufferHandle_t makeBuffer(size_t bytes, StaticStreamBuffer_t* state) {
  // 静态缓冲的存储区比容量多 1 字节；优先放 PSRAM，把内部 RAM 留给 BLE。
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!storage) storage = static_cast<uint8_t*>(heap_caps_malloc(bytes + 1, MALLOC_CAP_8BIT));
  return storage ? xStreamBufferCreateStatic(bytes, 1, storage, state) : nullptr;
}

void speakerTask(void*) {
  int16_t pcm[256];
  bool empty = true;
  for (;;) {
    if (flushSpeaker.load(std::memory_order_acquire)) {
      while (xStreamBufferReceive(playBuf, pcm, sizeof(pcm), 0) > 0) {}
      flushSpeaker.store(false, std::memory_order_release);
    }
    const size_t got = xStreamBufferReceive(playBuf, pcm, sizeof(pcm), pdMS_TO_TICKS(20));
    if (got < sizeof(int16_t)) {
      // TTS 还没结束缓冲就空了：下行速度跟不上播放，记一次欠载。
      if (!empty && ttsStreaming.load(std::memory_order_acquire)) speakerUnderruns.fetch_add(1);
      empty = true;
      continue;
    }
    empty = false;
    size_t written = 0;
    i2s_channel_write(txChan, pcm, got & ~size_t(1), &written, portMAX_DELAY);
  }
}

void micTask(void*) {
  int16_t pcm[kFrameSamples];
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rxChan, pcm, sizeof(pcm), &got, portMAX_DELAY) != ESP_OK || got != sizeof(pcm)) continue;
    if (!micCapture.load(std::memory_order_acquire)) continue;
    // 整帧写入或整帧丢弃，保证 loop 读到的永远是对齐的 20 ms 帧。
    if (xStreamBufferSpacesAvailable(micBuf) >= got) xStreamBufferSend(micBuf, pcm, got, 0);
    else micOverflowBytes.fetch_add(static_cast<uint32_t>(got));
  }
}

// ============================================================================
// 测试状态
// ============================================================================

bool started = false;
bool audioReady = false;
bool cameraReady = false;
esp_err_t startError = ESP_OK;
esp_err_t cocError = ESP_ERR_INVALID_STATE;  // NOT_SUPPORTED = Core 未启用 CoC
esp_err_t i2sError = ESP_OK;
esp_err_t cameraError = ESP_OK;
bool es8311Ok = false;
bool es7210Ok = false;

// 回调只置标志，统一在 loop 中处理和打印。
bool linkLost = false;
bool listenStart = false;
bool listenStop = false;
uint32_t listenMaxMs = 0;
bool captureRequested = false;
bool audioEndPending = false;

enum class UpPhase : uint8_t { Idle, Opening, Running, Closing, Draining };
struct UplinkTest {
  UpPhase phase = UpPhase::Idle;
  agent_stream_t kind = AGENT_STREAM_VOICE;
  agent_stream_handle_t handle = nullptr;
  bool localOnly = false;  // 'm'：只测麦克风，不发蓝牙
  bool complete = true;
  bool stopRequested = false;
  uint32_t maxMs = 0, requestedAt = 0, startedAt = 0, stoppedAt = 0, lastTry = 0;
  uint32_t framesSent = 0, framesDropped = 0, micOverflowAtStart = 0;
  int32_t peak = 0;
  double sumSquares = 0;
  uint32_t samples = 0;
};
UplinkTest up;

enum class ImgPhase : uint8_t { Idle, Open, Write, Close, Draining };
struct ImageTest {
  ImgPhase phase = ImgPhase::Idle;
  agent_stream_handle_t handle = nullptr;
  uint8_t* jpg = nullptr;
  size_t length = 0, offset = 0;
  uint16_t width = 0, height = 0;
  bool complete = true;
  uint32_t captureMs = 0, encodeMs = 0, requestedAt = 0, openedAt = 0, closedAt = 0, lastTry = 0;
};
ImageTest img;

struct DownlinkTest {
  bool active = false;
  uint32_t firstAt = 0, lastAt = 0;
  uint64_t bytes = 0;
  int32_t peak = 0;
  uint32_t underrunsAtStart = 0, backpressureAtStart = 0;
};
DownlinkTest dl;

uint32_t toneSamplesLeft = 0;
uint32_t tonePhase = 0;
const int16_t kSine1k[16] = {0, 3061, 5657, 7391, 8000, 7391, 5657, 3061,
                             0, -3061, -5657, -7391, -8000, -7391, -5657, -3061};

const char* kindName(agent_stream_t kind) {
  switch (kind) {
    case AGENT_STREAM_VOICE: return "VOICE(GATT 0xFFA1)";
    case AGENT_STREAM_AUDIO: return "AUDIO(CoC 0x0081)";
    case AGENT_STREAM_IMAGE: return "IMAGE(CoC 0x0082)";
    default: return "STREAM";
  }
}

const char* streamStateName(agent_stream_state_t state) {
  switch (state) {
    case AGENT_STREAM_UNAVAILABLE: return "UNAVAILABLE";
    case AGENT_STREAM_IDLE: return "IDLE";
    case AGENT_STREAM_OPEN: return "OPEN";
    case AGENT_STREAM_DRAINING: return "DRAINING";
    case AGENT_STREAM_COMPLETED: return "COMPLETED";
    case AGENT_STREAM_FAILED: return "FAILED";
  }
  return "?";
}

float toDbfs(double value) { return value > 0 ? 20.0f * log10f(static_cast<float>(value / 32768.0)) : -120.0f; }

float kbps(uint64_t bytes, uint32_t ms) { return ms ? static_cast<float>(bytes) / static_cast<float>(ms) : 0.0f; }

// ============================================================================
// 库回调（都在 Deotaland.poll() 内、loop 线程上执行）
// ============================================================================

void onState(agent_state_t state, void*) {
  // 非 READY（包括同一次 poll 内的断开重连）都会让旧的流句柄失效。
  if (state != AGENT_STATE_READY) linkLost = true;
}

void onListen(bool start, uint32_t maxMs, void*) {
  if (start) {
    listenStart = true;
    listenStop = false;
    listenMaxMs = maxMs;
  } else {
    listenStop = true;
    listenStart = false;
  }
}

void onAudioEnd(void*) {
  // END 表示所有 PCM 已进入本地缓冲；扬声器任务会继续把尾巴播完。
  ttsStreaming.store(false, std::memory_order_release);
  audioEndPending = true;
}

void onCameraCmd(const char*, const uint8_t* args, size_t length, void*) {
  if (length == 1 && args[0]) captureRequested = true;
}

size_t acceptPcm(const uint8_t* pcm, size_t bytes, void*) {
  if (toneSamplesLeft) return 0;  // 本地测试音播放期间暂缓下行，库会稍后重试
  size_t accepted = xStreamBufferSpacesAvailable(playBuf) & ~size_t(1);
  if (accepted > bytes) accepted = bytes & ~size_t(1);
  if (!accepted) return 0;
  accepted = xStreamBufferSend(playBuf, pcm, accepted, 0) & ~size_t(1);
  const uint32_t now = millis();
  if (!dl.active) {
    const agent_playback_stats_t stats = Deotaland.playbackStats();
    dl = DownlinkTest();
    dl.active = true;
    dl.firstAt = now;
    dl.underrunsAtStart = speakerUnderruns.load();
    dl.backpressureAtStart = stats.backpressure_events;
    ttsStreaming.store(true, std::memory_order_release);
  }
  dl.lastAt = now;
  dl.bytes += accepted;
  for (size_t i = 0; i + 1 < accepted; i += 2) {
    int16_t sample;
    memcpy(&sample, pcm + i, sizeof(sample));
    const int32_t magnitude = sample < 0 ? -static_cast<int32_t>(sample) : sample;
    if (magnitude > dl.peak) dl.peak = magnitude;
  }
  return accepted;
}

size_t queuedPcm(void*) {
  // 未播放的字节：应用缓冲 + I2S DMA（6 x 240 帧 x 2 字节）。
  return xStreamBufferBytesAvailable(playBuf) + 6u * 240u * 2u;
}

agent_output_cb_t callbacks = {};
agent_link_io_desc_t cameraIo = {};

// ============================================================================
// 报告
// ============================================================================

void printDownlinkReport(bool interrupted) {
  const agent_playback_stats_t stats = Deotaland.playbackStats();
  const uint32_t spanMs = dl.lastAt - dl.firstAt;
  const uint32_t audioMs = static_cast<uint32_t>(dl.bytes / 32u);
  Serial.printf("[TTS 下行]%s 收到 %u B（%u.%02u s 音频），接收用时 %u ms，%.1f KB/s，实时倍率 %.2fx\n",
                interrupted ? " 连接中断！" : "", static_cast<unsigned>(dl.bytes), static_cast<unsigned>(audioMs / 1000),
                static_cast<unsigned>(audioMs % 1000 / 10), static_cast<unsigned>(spanMs), kbps(dl.bytes, spanMs),
                spanMs ? static_cast<float>(audioMs) / static_cast<float>(spanMs) : 0.0f);
  const uint32_t underruns = speakerUnderruns.load() - dl.underrunsAtStart;
  Serial.printf("           播放欠载 %u 次，背压 %u 次，库内最大积压 %u B，峰值 %.1f dBFS\n", static_cast<unsigned>(underruns),
                static_cast<unsigned>(stats.backpressure_events - dl.backpressureAtStart),
                static_cast<unsigned>(stats.max_pending_pcm_bytes), toDbfs(dl.peak));
  if (underruns) Serial.println("           有欠载：下行速度跟不上实时播放，检查连接间隔/MTU/信号。");
  if (dl.peak == 0) Serial.println("           收到的 PCM 全为 0：检查 App 下发内容。");
}

void printMicLevel() {
  const double rms = up.samples ? sqrt(up.sumSquares / up.samples) : 0;
  Serial.printf("           麦克风峰值 %.1f dBFS，RMS %.1f dBFS，采集溢出 %u B\n", toDbfs(up.peak), toDbfs(rms),
                static_cast<unsigned>(micOverflowBytes.load() - up.micOverflowAtStart));
  if (up.peak < 33) Serial.println("           几乎没有信号：检查 ES7210 初始化、增益或麦克风。");
}

void printUplinkReport(const agent_stream_status_t& status, esp_err_t statusError, uint32_t now) {
  const uint32_t captureMs = up.stoppedAt - up.startedAt;
  Serial.printf("[%s 上行] 采集 %u ms：发送 %u 帧（%u B），拥塞丢弃 %u 帧\n", kindName(up.kind),
                static_cast<unsigned>(captureMs), static_cast<unsigned>(up.framesSent),
                static_cast<unsigned>(up.framesSent * kFrameBytes), static_cast<unsigned>(up.framesDropped));
  printMicLevel();
  if (statusError != ESP_OK) {
    Serial.printf("           无法读取通道状态: %s\n", Deotaland.errorName(statusError));
    return;
  }
  const uint32_t totalMs = now - up.startedAt;
  Serial.printf("           通道 accepted=%u B sent=%u B 状态=%s last_error=%s，关闭后排空 %u ms，平均 %.1f KB/s\n",
                static_cast<unsigned>(status.accepted_bytes), static_cast<unsigned>(status.sent_bytes),
                streamStateName(status.state), Deotaland.errorName(status.last_error),
                static_cast<unsigned>(now - up.stoppedAt), kbps(status.sent_bytes, totalMs));
  if (up.framesDropped) Serial.println("           有丢帧：上行速度跟不上实时采集（32 KB/s）。");
}

void printImageReport(const agent_stream_status_t& status, esp_err_t statusError, uint32_t now, bool timedOut) {
  const uint32_t uploadMs = now - img.openedAt;
  Serial.printf("[IMAGE 上行] %ux%u JPEG %u B（采集 %u ms，编码 %u ms），上传 %u ms，%.1f KB/s\n",
                static_cast<unsigned>(img.width), static_cast<unsigned>(img.height), static_cast<unsigned>(img.length),
                static_cast<unsigned>(img.captureMs), static_cast<unsigned>(img.encodeMs), static_cast<unsigned>(uploadMs),
                kbps(img.length, uploadMs));
  if (statusError != ESP_OK) {
    Serial.printf("             无法读取通道状态: %s\n", Deotaland.errorName(statusError));
    return;
  }
  Serial.printf("             通道 sent=%u B 状态=%s last_error=%s%s\n", static_cast<unsigned>(status.sent_bytes),
                streamStateName(status.state), Deotaland.errorName(status.last_error), timedOut ? "（等待超时）" : "");
}

void printStreamStatus(agent_stream_t kind) {
  agent_stream_status_t status = {};
  const esp_err_t result = Deotaland.streamStatus(kind, &status);
  if (result != ESP_OK) {
    Serial.printf("  %-20s %s\n", kindName(kind), Deotaland.errorName(result));
    return;
  }
  Serial.printf("  %-20s %s accepted=%u sent=%u queued=%u last_error=%s\n", kindName(kind),
                streamStateName(status.state), static_cast<unsigned>(status.accepted_bytes),
                static_cast<unsigned>(status.sent_bytes), static_cast<unsigned>(status.queued_bytes),
                Deotaland.errorName(status.last_error));
}

void printStatus() {
  Serial.println("---- KorvoMediaTest 状态 ----");
  if (startError != ESP_OK) {
    Serial.printf("启动失败: %s，%s\n", Deotaland.errorName(startError), Deotaland.errorHint(startError));
  }
  Serial.printf("硬件: I2S=%s ES8311=%s ES7210=%s 摄像头=%s(PID=0x%04X)\n", Deotaland.errorName(i2sError),
                es8311Ok ? "OK" : "失败", es7210Ok ? "OK" : "失败",
                cameraReady ? "OK" : Deotaland.errorName(cameraError), static_cast<unsigned>(cameraPid));
  Serial.printf("链路: %s，ATT MTU=%u\n", Deotaland.stateName(Deotaland.state()),
                static_cast<unsigned>(agent_transport_ble_att_mtu()));
  if (cocError == ESP_ERR_NOT_SUPPORTED)
    Serial.println("CoC: 当前 Core 未启用，TTS 下行、AUDIO、IMAGE 不可用；只能测 VOICE 上行。");
  else if (cocError != ESP_OK)
    Serial.printf("CoC: 能力检查失败 %s\n", Deotaland.errorName(cocError));
  else
    Serial.println("CoC: Core 支持；各通道仍需 App 打开（PSM 0x0081 音频、0x0082 图片）。");
  printStreamStatus(AGENT_STREAM_VOICE);
  printStreamStatus(AGENT_STREAM_AUDIO);
  printStreamStatus(AGENT_STREAM_IMAGE);
  const agent_playback_stats_t playback = Deotaland.playbackStats();
  Serial.printf("播放: 累计 %u B，背压 %u 次，库内积压 %u B，本地缓冲 %u B，欠载 %u 次\n",
                static_cast<unsigned>(playback.accepted_pcm_bytes), static_cast<unsigned>(playback.backpressure_events),
                static_cast<unsigned>(playback.pending_pcm_bytes),
                static_cast<unsigned>(playBuf ? xStreamBufferBytesAvailable(playBuf) : 0),
                static_cast<unsigned>(speakerUnderruns.load()));
  const agent_link_stats_t stats = Deotaland.stats();
  Serial.printf("统计: rx_dropped=%u tx_retries=%u last_error=%s\n", static_cast<unsigned>(stats.rx_dropped),
                static_cast<unsigned>(stats.tx_retries), Deotaland.errorName(stats.last_error));
  Serial.printf("内存: 内部 %u B，PSRAM %u B\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  Serial.println("命令: v=VOICE上行 a=AUDIO上行 c=拍照上行 t=扬声器自检 m=麦克风自检 ?=状态");
}

// ============================================================================
// 语音上行（VOICE / AUDIO）和本地麦克风自检
// ============================================================================

void beginCapture(uint32_t now) {
  micCapture.store(false, std::memory_order_release);
  uint8_t discard[kFrameBytes];
  while (xStreamBufferReceive(micBuf, discard, sizeof(discard), 0) > 0) {}
  up.micOverflowAtStart = micOverflowBytes.load();
  up.startedAt = now;
  up.phase = UpPhase::Running;
  micCapture.store(true, std::memory_order_release);
}

void startUplink(agent_stream_t kind, uint32_t maxMs, bool localOnly) {
  if (!audioReady) {
    Serial.println("音频硬件未就绪，无法采集麦克风；输入 ? 查看初始化结果。");
    return;
  }
  if (up.phase != UpPhase::Idle) {
    Serial.println("已有麦克风测试在进行，先结束它。");
    return;
  }
  if (!localOnly && !Deotaland.isReady()) {
    Serial.println("蓝牙未 READY：先用 App 连接并订阅 0xFFC4。");
    return;
  }
  if (!localOnly && kind == AGENT_STREAM_AUDIO && cocError != ESP_OK) {
    Serial.println("AUDIO 需要 CoC，当前 Core 不支持。");
    return;
  }
  const uint32_t now = millis();
  up = UplinkTest();
  up.kind = kind;
  up.maxMs = maxMs;
  up.localOnly = localOnly;
  up.requestedAt = now;
  up.lastTry = now - 100;
  if (localOnly) {
    beginCapture(now);
    Serial.println("麦克风自检：请对着麦克风说话，1 秒后给出电平。");
  } else {
    up.phase = UpPhase::Opening;
  }
}

void stopUplink(agent_stream_t kind) {
  if (up.phase == UpPhase::Opening || up.phase == UpPhase::Running) {
    if (up.kind == kind) up.stopRequested = true;
  }
}

void accumulateLevel(const int16_t* pcm, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const int32_t sample = pcm[i];
    const int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > up.peak) up.peak = magnitude;
    up.sumSquares += static_cast<double>(sample) * sample;
  }
  up.samples += count;
}

void serviceUplink(uint32_t now) {
  switch (up.phase) {
    case UpPhase::Idle:
      return;
    case UpPhase::Opening: {
      if (now - up.lastTry < 100) return;
      up.lastTry = now;
      agent_stream_opts_t opts = {};
      opts.name = "korvo-mic";
      const esp_err_t result =
          Deotaland.openStream(up.kind, up.kind == AGENT_STREAM_AUDIO ? &opts : nullptr, &up.handle);
      if (result == ESP_OK) {
        beginCapture(now);
        Serial.printf("[%s 上行] 开始，最长 %u ms\n", kindName(up.kind), static_cast<unsigned>(up.maxMs));
        return;
      }
      // READY 只代表控制通道；App 可能还在订阅 0xFFA1 或打开 CoC，给它 2 秒。
      if ((retryable(result) || result == ESP_ERR_INVALID_STATE) && !up.stopRequested && now - up.requestedAt < 2000)
        return;
      Serial.printf("[%s 上行] 打开失败: %s\n", kindName(up.kind), Deotaland.errorName(result));
      if (result == ESP_ERR_INVALID_STATE)
        Serial.println(up.kind == AGENT_STREAM_VOICE ? "           App 未订阅 0xFFA1。"
                                                     : "           App 未打开音频 CoC 通道（PSM 0x0081）。");
      else if (result == ESP_ERR_NOT_SUPPORTED)
        Serial.println("           当前 Core 不支持该通道。");
      up.phase = UpPhase::Idle;
      return;
    }
    case UpPhase::Running: {
      int16_t frame[kFrameSamples];
      for (int n = 0; n < 4 && xStreamBufferBytesAvailable(micBuf) >= kFrameBytes; ++n) {
        if (xStreamBufferReceive(micBuf, frame, kFrameBytes, 0) != kFrameBytes) break;
        accumulateLevel(frame, kFrameSamples);
        if (up.localOnly) {
          ++up.framesSent;
          continue;
        }
        const esp_err_t result = Deotaland.writeStream(up.handle, frame, kFrameBytes);
        if (result == ESP_OK) {
          ++up.framesSent;
        } else if (retryable(result)) {
          // 实时音频不能等待：队列满就丢这一帧，并记为不完整。
          ++up.framesDropped;
          up.complete = false;
        } else {
          Serial.printf("[%s 上行] 写入失败: %s\n", kindName(up.kind), Deotaland.errorName(result));
          up.complete = false;
          up.stopRequested = true;
          break;
        }
      }
      const bool linkOk = up.localOnly || Deotaland.isReady();
      if (!up.stopRequested && linkOk && now - up.startedAt < up.maxMs) return;
      micCapture.store(false, std::memory_order_release);
      up.stoppedAt = now;
      if (up.localOnly) {
        Serial.printf("[麦克风自检] 采集 %u 帧\n", static_cast<unsigned>(up.framesSent));
        printMicLevel();
        up.phase = UpPhase::Idle;
        return;
      }
      up.complete = up.complete && linkOk;
      up.phase = UpPhase::Closing;
      up.lastTry = now - 20;
      return;
    }
    case UpPhase::Closing: {
      if (now - up.lastTry < 20) return;
      up.lastTry = now;
      const esp_err_t result = Deotaland.closeStream(up.handle, up.complete);
      if (retryable(result)) return;
      up.handle = nullptr;
      if (result != ESP_OK) {
        Serial.printf("[%s 上行] 关闭失败: %s\n", kindName(up.kind), Deotaland.errorName(result));
        up.phase = UpPhase::Idle;
        return;
      }
      up.phase = UpPhase::Draining;
      return;
    }
    case UpPhase::Draining: {
      agent_stream_status_t status = {};
      const esp_err_t result = Deotaland.streamStatus(up.kind, &status);
      const bool busy =
          result == ESP_OK && (status.state == AGENT_STREAM_OPEN || status.state == AGENT_STREAM_DRAINING);
      if (busy && now - up.stoppedAt < 10000) return;
      printUplinkReport(status, result, now);
      up.phase = UpPhase::Idle;
      return;
    }
  }
}

// ============================================================================
// 图片上行
// ============================================================================

void releaseImage() {
  free(img.jpg);
  img.jpg = nullptr;
  img.handle = nullptr;
  img.phase = ImgPhase::Idle;
}

void startCapture() {
  if (!cameraReady) {
    Serial.println("摄像头未就绪；输入 ? 查看初始化结果。");
    return;
  }
  if (img.phase != ImgPhase::Idle) {
    Serial.println("上一张图片还在上传。");
    return;
  }
  if (!Deotaland.isReady()) {
    Serial.println("蓝牙未 READY：先用 App 连接并订阅 0xFFC4。");
    return;
  }
  if (cocError != ESP_OK) {
    Serial.println("IMAGE 需要 CoC，当前 Core 不支持。");
    return;
  }
  const uint32_t t0 = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) {
    // CAMERA_GRAB_LATEST 队列里先出来的是较旧的一帧，丢掉再取一帧。
    esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
  }
  if (!fb) {
    Serial.println("[IMAGE 上行] 取帧失败");
    return;
  }
  maskTopRows(fb);
  const uint32_t t1 = millis();
  img = ImageTest();
  const bool encoded = frame2jpg(fb, kJpegQuality, &img.jpg, &img.length);
  img.width = static_cast<uint16_t>(fb->width);
  img.height = static_cast<uint16_t>(fb->height);
  esp_camera_fb_return(fb);
  const uint32_t t2 = millis();
  if (!encoded || !img.jpg || !img.length) {
    Serial.println("[IMAGE 上行] JPEG 编码失败（PSRAM 不足？）");
    releaseImage();
    return;
  }
  img.captureMs = t1 - t0;
  img.encodeMs = t2 - t1;
  img.requestedAt = t2;
  img.lastTry = t2 - 20;
  img.phase = ImgPhase::Open;
}

void serviceImage(uint32_t now) {
  switch (img.phase) {
    case ImgPhase::Idle:
      return;
    case ImgPhase::Open: {
      if (now - img.lastTry < 20) return;
      img.lastTry = now;
      agent_stream_opts_t opts = {};
      opts.encoding = AGENT_ENC_JPEG;
      opts.width = img.width;
      opts.height = img.height;
      opts.total_bytes = static_cast<uint32_t>(img.length);
      const esp_err_t result = Deotaland.openStream(AGENT_STREAM_IMAGE, &opts, &img.handle);
      if (result == ESP_OK) {
        img.openedAt = now;
        img.phase = ImgPhase::Write;
        return;
      }
      if ((retryable(result) || result == ESP_ERR_INVALID_STATE) && now - img.requestedAt < 3000) return;
      Serial.printf("[IMAGE 上行] 打开失败: %s\n", Deotaland.errorName(result));
      if (result == ESP_ERR_INVALID_STATE) Serial.println("             App 未打开图片 CoC 通道（PSM 0x0082）。");
      releaseImage();
      return;
    }
    case ImgPhase::Write: {
      for (int n = 0; n < 8 && img.offset < img.length; ++n) {
        size_t chunk = img.length - img.offset;
        if (chunk > kImageChunkBytes) chunk = kImageChunkBytes;
        const esp_err_t result = Deotaland.writeStream(img.handle, img.jpg + img.offset, chunk);
        if (retryable(result)) return;  // 队列满：保留偏移，下轮重试同一块
        if (result != ESP_OK) {
          Serial.printf("[IMAGE 上行] 写入失败: %s\n", Deotaland.errorName(result));
          img.complete = false;
          break;
        }
        img.offset += chunk;
      }
      if (img.complete && img.offset < img.length) return;
      img.phase = ImgPhase::Close;
      img.lastTry = now - 20;
      return;
    }
    case ImgPhase::Close: {
      if (now - img.lastTry < 20) return;
      img.lastTry = now;
      const esp_err_t result = Deotaland.closeStream(img.handle, img.complete);
      if (retryable(result)) return;
      if (result != ESP_OK) {
        Serial.printf("[IMAGE 上行] 关闭失败: %s\n", Deotaland.errorName(result));
        releaseImage();
        return;
      }
      // 数据已全部复制进库的发送队列，可以释放 JPEG。
      free(img.jpg);
      img.jpg = nullptr;
      img.handle = nullptr;
      img.closedAt = now;
      img.phase = ImgPhase::Draining;
      return;
    }
    case ImgPhase::Draining: {
      agent_stream_status_t status = {};
      const esp_err_t result = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
      const bool busy =
          result == ESP_OK && (status.state == AGENT_STREAM_OPEN || status.state == AGENT_STREAM_DRAINING);
      const bool timedOut = busy && now - img.closedAt >= 15000;
      if (busy && !timedOut) return;
      printImageReport(status, result, now, timedOut);
      releaseImage();
      return;
    }
  }
}

// ============================================================================
// 本地扬声器自检、链路事件、串口命令
// ============================================================================

void startTone() {
  if (!audioReady) {
    Serial.println("音频硬件未就绪；输入 ? 查看初始化结果。");
    return;
  }
  if (dl.active || toneSamplesLeft) {
    Serial.println("正在播放，稍后再试。");
    return;
  }
  toneSamplesLeft = kSampleRate;  // 1 秒
  Serial.println("扬声器自检：播放 1 kHz 测试音 1 秒。");
}

void serviceTone() {
  if (!toneSamplesLeft) return;
  int16_t chunk[256];
  size_t count = xStreamBufferSpacesAvailable(playBuf) / sizeof(int16_t);
  if (count > 256) count = 256;
  if (count > toneSamplesLeft) count = toneSamplesLeft;
  if (!count) return;
  for (size_t i = 0; i < count; ++i) chunk[i] = kSine1k[tonePhase++ & 15u];
  xStreamBufferSend(playBuf, chunk, count * sizeof(int16_t), 0);
  toneSamplesLeft -= count;
}

void handleLinkEvents() {
  if (linkLost) {
    linkLost = false;
    listenStart = listenStop = false;
    captureRequested = false;
    if (up.phase != UpPhase::Idle && !up.localOnly) {
      micCapture.store(false, std::memory_order_release);
      Serial.printf("[%s 上行] 连接断开，测试中止\n", kindName(up.kind));
      up.phase = UpPhase::Idle;
      up.handle = nullptr;
    }
    if (img.phase != ImgPhase::Idle) {
      Serial.println("[IMAGE 上行] 连接断开，测试中止");
      releaseImage();
    }
    if (dl.active) {
      ttsStreaming.store(false, std::memory_order_release);
      printDownlinkReport(true);
      dl.active = false;
    }
    audioEndPending = false;
    flushSpeaker.store(true, std::memory_order_release);  // 不把旧会话的语音带到下一次连接
  }
  if (audioEndPending) {
    audioEndPending = false;
    if (dl.active) printDownlinkReport(false);
    dl.active = false;
  }
  if (listenStart) {
    listenStart = false;
    if (up.phase == UpPhase::Idle)
      startUplink(AGENT_STREAM_AUDIO, listenMaxMs ? listenMaxMs : kAudioMaxMs, false);
    else
      Serial.println("App 请求开始采集，但麦克风正被其他测试占用，已忽略。");
  }
  if (listenStop) {
    listenStop = false;
    stopUplink(AGENT_STREAM_AUDIO);
  }
  if (captureRequested) {
    captureRequested = false;
    startCapture();
  }
}

void toggleUplink(agent_stream_t kind, uint32_t maxMs) {
  if (up.phase != UpPhase::Idle && up.kind == kind && !up.localOnly) stopUplink(kind);
  else startUplink(kind, maxMs, false);
}

void handleSerial() {
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n) {
    switch (Serial.read()) {
      case 'v': case 'V': toggleUplink(AGENT_STREAM_VOICE, kVoiceMaxMs); break;
      case 'a': case 'A': toggleUplink(AGENT_STREAM_AUDIO, kAudioMaxMs); break;
      case 'c': case 'C': startCapture(); break;
      case 't': case 'T': startTone(); break;
      case 'm': case 'M': startUplink(AGENT_STREAM_AUDIO, kMicTestMs, true); break;
      case '?': printStatus(); break;
      default: break;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("KorvoMediaTest：BLE 语音上行 / TTS 下行 / 图片上行联调");

  // 1. I2C（Wire = I2C0），摄像头和两颗编解码器都挂在这条总线上。
  Wire.begin(kI2cSda, kI2cScl, 100000);

  // 2. 音频：I2S 时钟先跑起来，再配置编解码器，最后打开功放。
  playBuf = makeBuffer(kPlayBufBytes, &playBufState);
  micBuf = makeBuffer(kMicBufBytes, &micBufState);
  i2sError = (playBuf && micBuf) ? initI2s() : ESP_ERR_NO_MEM;
  if (i2sError == ESP_OK) {
    es8311Ok = initEs8311();
    es7210Ok = initEs7210();
    pinMode(kPaEnablePin, OUTPUT);
    digitalWrite(kPaEnablePin, es8311Ok ? HIGH : LOW);
    audioReady = es8311Ok && es7210Ok;
    if (audioReady) {
      xTaskCreatePinnedToCore(speakerTask, "korvo_spk", 4096, nullptr, 5, nullptr, 1);
      xTaskCreatePinnedToCore(micTask, "korvo_mic", 4096, nullptr, 5, nullptr, 1);
    }
  }

  // 3. 摄像头。
  cameraError = initCamera();
  cameraReady = cameraError == ESP_OK;

  // 4. 蓝牙。camera0 端点让 App 也能触发拍照；端点必须在 begin 前注册。
  cameraIo.id = "camera0";
  cameraIo.dir = AGENT_IO_OUT;
  cameraIo.kind = "camera.capture";
  cameraIo.value = AGENT_VAL_BOOL;
  cameraIo.desc = "Capture a still image and upload it to the app";
  cameraIo.display_name = "Camera Snapshot";
  if (cameraReady) {
    const esp_err_t result = Deotaland.registerIo(cameraIo, onCameraCmd);
    if (result != ESP_OK) Serial.printf("registerIo(camera0): %s\n", Deotaland.errorName(result));
  }
  callbacks.on_audio_end = onAudioEnd;
  callbacks.on_listen = onListen;
  agent_link_config_t config = {};
  config.device_name = "Deotaland-Korvo";
  config.caps = AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_CAMERA;
  config.output = &callbacks;
  config.on_state = onState;
  config.firmware_rev = "2.0.0";
  startError = Deotaland.begin(config);
  started = startError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    // 内置 BLE 的 AUDIO、IMAGE 与 TTS 共用 CoC 编译能力；只看返回码，连接前的状态不代表能力。
    cocError = Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status);
    esp_err_t result = Deotaland.prepareStreams(1u << AGENT_STREAM_VOICE);
    if (result != ESP_OK) Serial.printf("prepareStreams(VOICE): %s\n", Deotaland.errorName(result));
    if (cocError == ESP_OK) {
      result = Deotaland.prepareStreams((1u << AGENT_STREAM_AUDIO) | (1u << AGENT_STREAM_IMAGE));
      if (result != ESP_OK) Serial.printf("prepareStreams(AUDIO|IMAGE): %s\n", Deotaland.errorName(result));
      if (audioReady) {
        agent_playback_config_t playback = {};
        playback.write = acceptPcm;
        playback.queued = queuedPcm;
        result = Deotaland.configurePlayback(&playback);
        if (result != ESP_OK) Serial.printf("configurePlayback: %s\n", Deotaland.errorName(result));
        // 声明真实能缓冲的时长，库据此给 App 发暂停/恢复（事件 0x20）。
        Deotaland.setPlaybackBufferMs(kPlayBufBytes / 32);
      }
    }
  }
  printStatus();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  handleLinkEvents();
  handleSerial();
  serviceUplink(now);
  serviceImage(now);
  serviceTone();
}
