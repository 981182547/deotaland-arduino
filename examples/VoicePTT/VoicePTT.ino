/* 语音进阶：默认使用串口 p/r 开始/结束，115200 波特率，无需接线。
 * 本例发送合成测试音，不采集人声；收到的 TTS 只计数，不驱动扬声器。
 * 真正对话须替换 makeTestPcm 和播放驱动。VOICE 需订阅 0xFFA1；TTS 需 CoC。
 * 使用实体按键时设置 kPttPin；按下接 GND，INPUT_PULLUP，避免猜测板上 BOOT 引脚。
 */
#include <Deotaland.h>

namespace {

constexpr int kPttPin = -1;  // 默认禁用 GPIO；按你的板卡填写真实按键引脚。
bool started = false;
esp_err_t startError = ESP_OK, prepareError = ESP_ERR_INVALID_STATE, ttsSupportError = ESP_ERR_INVALID_STATE;
uint32_t lastDiagnostic = 0;
bool diagnosticPending = true;
bool talking = false;
bool serial_ptt = false;
bool raw_button = false;
bool stable_button = false;
bool closing = false;
bool draining = false;
bool complete_voice = true;
bool wait_release = false;
bool stream_fault = false;
agent_stream_handle_t voice_stream = nullptr;
uint32_t button_changed_at = 0;
uint32_t last_pcm_at = 0;
uint32_t last_close_at = 0;
uint32_t last_open_at = 0;
size_t tts_bytes = 0;

// Replace this test signal with 16 kHz, signed 16-bit, mono microphone PCM.
void makeTestPcm(int16_t* samples, size_t count) {
  static uint16_t phase = 0;
  for (size_t i = 0; i < count; ++i) {
    phase += 900;
    samples[i] = (phase & 0x8000) ? 900 : -900;
  }
}

void onAudio(const uint8_t*, size_t bytes, void*) {
  // Called from poll(): queue PCM to the board's speaker without waiting for playback.
  // This demo has no speaker driver. Count/discard the test audio; logging every
  // chunk here could fill the UART buffer and block the receive path.
  tts_bytes += bytes;
}

void onAudioEnd(void*) { tts_bytes = 0; }

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    // Even a disconnect + reconnect in one poll invalidates the old handle.
    talking = closing = draining = false;
    stream_fault = false;
    voice_stream = nullptr;
    wait_release = true;
    serial_ptt = false;
  }
}

agent_output_cb_t callbacks = {};

void printDiagnostic() {
  Serial.println("VoicePTT：p/r 开始/结束合成测试音，无麦克风或扬声器驱动；? 可重看诊断。");
  if (startError != ESP_OK) {
    Serial.print("启动失败: "); Serial.println(Deotaland.errorName(startError));
    Serial.println(Deotaland.errorHint(startError));
  } else if (prepareError != ESP_OK) {
    Serial.print("VOICE 准备失败，已停止语音任务: "); Serial.println(Deotaland.errorName(prepareError));
    Serial.println(Deotaland.errorHint(prepareError));
  } else {
    Serial.print("控制状态: "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.println("VOICE 上传需 App 订阅 0xFFA1；收到的 TTS 仅计数并丢弃，不会播放。");
    if (ttsSupportError == ESP_ERR_NOT_SUPPORTED)
      Serial.println("当前后端不支持 CoC，TTS 下行不可用。检查本库自带的 NimBLE 配置；VOICE 上传无需 CoC。");
    else if (ttsSupportError != ESP_OK) {
      Serial.print("TTS/CoC 能力检查失败: "); Serial.println(Deotaland.errorName(ttsSupportError));
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  if (kPttPin >= 0) pinMode(kPttPin, INPUT_PULLUP);
  callbacks.on_audio_out = onAudio;
  callbacks.on_audio_end = onAudioEnd;

  agent_link_config_t config = {};
  config.device_name = "Deotaland-Voice";
  config.caps = AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_BUTTON;
  config.output = &callbacks;
  config.on_state = onState;
  config.firmware_rev = "2.0.0";
  startError = Deotaland.begin(config);
  started = startError == ESP_OK;
  if (started) {
    // Allocate the VOICE ring and persistent worker during setup. After this
    // succeeds, capture sessions need no task creation or ring allocation.
    prepareError = Deotaland.prepareStreams(1u << AGENT_STREAM_VOICE);
    agent_stream_status_t status = {};
    // 内置 BLE 的 AUDIO 与 TTS 共用 CoC 能力；检测失败不影响 GATT VOICE 上传。
    ttsSupportError = Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status);
  }
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  if (draining) {
    // closeStream accepts END locally; the worker may still be sending audio.
    // Wait through that phase before trying to start the next utterance.
    agent_stream_status_t status = {};
    const esp_err_t result = Deotaland.streamStatus(AGENT_STREAM_VOICE, &status);
    if (result == ESP_OK) {
      draining = status.state == AGENT_STREAM_DRAINING || status.state == AGENT_STREAM_OPEN;
    } else if (result != ESP_ERR_NO_MEM && result != ESP_ERR_TIMEOUT) {
      draining = false;  // An optional CUSTOM backend may not expose status.
    }
  }
  for (uint8_t n = 0; n < 16 && Serial.available(); ++n) {
    const char command = static_cast<char>(Serial.read());
    if (command == '?') diagnosticPending = true;
    if (command == 'p' || command == 'P') {
      serial_ptt = true;
      Serial.println("Serial PTT pressed");
    }
    if (command == 'r' || command == 'R') {
      serial_ptt = false;
      Serial.println("Serial PTT released");
    }
  }
  // 连续输入 ? 也限频，避免串口日志挤占 poll。
  if ((diagnosticPending && uint32_t(now - lastDiagnostic) >= 500) ||
      uint32_t(now - lastDiagnostic) >= 10000) {
    diagnosticPending = false; lastDiagnostic = now;
    printDiagnostic();
  }
  if (!started || prepareError != ESP_OK) return;

  const bool button = kPttPin >= 0 && digitalRead(kPttPin) == LOW;
  if (button != raw_button) { raw_button = button; button_changed_at = now; }
  if (now - button_changed_at >= 20) stable_button = raw_button;
  const bool pressed = serial_ptt || stable_button;
  if (!pressed) wait_release = false;
  if (pressed && !talking && !closing && !draining && !wait_release && !stream_fault && Deotaland.isReady() &&
      now - last_open_at >= 100) {
    last_open_at = now;
    const esp_err_t result = Deotaland.openStream(AGENT_STREAM_VOICE, nullptr, &voice_stream);
    if (result == ESP_OK) {
      talking = true;
      complete_voice = true;
      last_pcm_at = now - 20;
      Serial.println("PTT started");
    } else if (result != ESP_ERR_NO_MEM && result != ESP_ERR_TIMEOUT &&
               result != ESP_ERR_INVALID_STATE) {
      // INVALID_STATE can mean the peer has not subscribed to voice 0xFFA1 yet.
      wait_release = true;
      Serial.printf("openStream(VOICE): %s\n", esp_err_to_name(result));
    }
  }

  if (talking && (!pressed || !Deotaland.isReady())) {
    talking = false;
    closing = true;
    complete_voice = complete_voice && Deotaland.isReady();
    last_close_at = now - 20;
  }
  if (closing && now - last_close_at >= 20) {
    last_close_at = now;
    const esp_err_t result = Deotaland.closeStream(voice_stream, complete_voice);
    if (result != ESP_ERR_NO_MEM && result != ESP_ERR_TIMEOUT) {
      closing = false;
      draining = result == ESP_OK;
      voice_stream = nullptr;
      // Permanent END errors require reconnect before starting another session.
      if (result != ESP_OK) stream_fault = true;
      Serial.printf("closeStream(VOICE): %s\n", esp_err_to_name(result));
    }
  }
  if (talking && now - last_pcm_at >= 20) {
    last_pcm_at = now;
    int16_t pcm[320];  // 20 ms at 16 kHz
    makeTestPcm(pcm, 320);
    const esp_err_t result = Deotaland.writeStream(voice_stream, pcm, sizeof(pcm));
    if (result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT) complete_voice = false;
    // Live audio may drop a chunk under backpressure; the next loop stays responsive.
    if (result != ESP_OK && result != ESP_ERR_NO_MEM && result != ESP_ERR_TIMEOUT) {
      Serial.printf("writeStream(VOICE): %s\n", esp_err_to_name(result));
      talking = false;
      closing = true;
      complete_voice = false;
      wait_release = true;
      last_close_at = now - 20;
    }
  }
}
