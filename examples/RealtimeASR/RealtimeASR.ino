/* 进阶 ASR 状态机：需 App 发起采集；本库自带启用 CoC 的 NimBLE 后端。
 * 这里发送合成测试 PCM，不采集麦克风。请替换 makeTestPcm 后再测试识别。
 * 使用兼容的官方 ESP32 Arduino Core；先用 BasicBLE 验证控制连接。
 */
#include <Deotaland.h>

namespace {

bool started = false;
esp_err_t startError = ESP_OK, supportError = ESP_ERR_INVALID_STATE;
uint32_t lastDiagnostic = 0;
bool diagnosticPending = true;
bool capture_requested = false;
uint32_t capture_started_at = 0;
uint32_t capture_duration = 0;
bool streaming = false;
bool closing = false;
bool complete_capture = true;
bool stream_fault = false;
agent_stream_handle_t audio_stream = nullptr;
uint32_t last_pcm_at = 0;
uint32_t last_close_at = 0;
uint32_t last_open_at = 0;

bool retryable(esp_err_t result) {
  return result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT;
}

void onListen(bool start, uint32_t max_ms, void*) {
  // Called from Deotaland.poll(), on the same thread as loop().
  if (supportError != ESP_OK) return; // 不为缺少 AUDIO 的后端启动无效采集任务。
  capture_requested = start;
  capture_started_at = millis();
  capture_duration = start ? max_ms : 0;
  last_open_at = capture_started_at - 100;
}

void onState(agent_state_t state, void*) {
  // A capture request belongs to its connection. Never resume it after reconnect.
  if (state != AGENT_STATE_READY) {
    capture_requested = false;
    capture_duration = 0;
    streaming = closing = false;
    stream_fault = false;
    audio_stream = nullptr;
  }
}

agent_output_cb_t callbacks = {};

// Replace this test tone with 16 kHz mono PCM captured from your microphone/codec.
void makeTestPcm(int16_t* samples, size_t count) {
  static uint16_t phase = 0;
  for (size_t i = 0; i < count; ++i) {
    phase += 700;
    samples[i] = (phase & 0x4000) ? 1200 : -1200;
  }
}

void printDiagnostic() {
  Serial.println("RealtimeASR：合成测试音，无麦克风驱动，不能用来识别人声；? 可重看诊断。");
  if (startError != ESP_OK) {
    Serial.print("启动失败: "); Serial.println(Deotaland.errorName(startError));
    Serial.println(Deotaland.errorHint(startError));
  } else if (supportError == ESP_ERR_NOT_SUPPORTED) {
    Serial.println("当前后端不支持 AUDIO/CoC，已停止采集任务。请检查 BLE 目标及本库自带的 NimBLE 配置，见 docs/BLE-COC.zh-CN.md。");
  } else if (supportError != ESP_OK) {
    Serial.print("AUDIO 能力检查失败: "); Serial.println(Deotaland.errorName(supportError));
    Serial.println(Deotaland.errorHint(supportError));
  } else {
    Serial.print("控制状态: "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.println("后端支持 AUDIO；仍需 App 打开 CoC 并发起采集。实际识别须替换 makeTestPcm。");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  callbacks.on_listen = onListen;
  agent_link_config_t config = {};
  config.device_name = "Deotaland-ASR";
  config.caps = AGENT_CAP_MIC | AGENT_CAP_RECORDING;
  config.output = &callbacks;
  config.on_state = onState;
  config.firmware_rev = "2.0.0";
  startError = Deotaland.begin(config);
  started = startError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    // 无需等 App 连接，也不分配流缓存。只看返回码：UNAVAILABLE 状态可能只是还未连接。
    supportError = Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status);
  }
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n)
    if (Serial.read() == '?') diagnosticPending = true;
  // 连续输入 ? 也限频，避免串口日志挤占 poll。
  if ((diagnosticPending && uint32_t(now - lastDiagnostic) >= 500) ||
      uint32_t(now - lastDiagnostic) >= 10000) {
    diagnosticPending = false; lastDiagnostic = now;
    printDiagnostic();
  }
  if (!started || supportError != ESP_OK) return;
  if (capture_duration && uint32_t(now - capture_started_at) >= capture_duration) {
    capture_requested = false;
    capture_duration = 0;
  }

  if (capture_requested && !streaming && !closing && !stream_fault && Deotaland.isReady() &&
      uint32_t(now - last_open_at) >= 100) {
    last_open_at = now;
    agent_stream_opts_t options = {};
    options.name = "arduino-asr";
    const esp_err_t result = Deotaland.openStream(AGENT_STREAM_AUDIO, &options, &audio_stream);
    // READY covers the control channel; the peer may still be opening its CoC.
    if (retryable(result) || result == ESP_ERR_INVALID_STATE) return;
    Serial.printf("openStream(AUDIO): %s\n", esp_err_to_name(result));
    streaming = result == ESP_OK;
    last_pcm_at = now - 20;
    complete_capture = true;
    if (!streaming) {
      // A failed start (for example, an unsupported transport backend) is a
      // completed capture attempt. Do not retry and flood the serial log on
      // every loop iteration; wait for the next StartCapture command.
      capture_requested = false;
      capture_duration = 0;
    }
  }
  if ((!capture_requested || !Deotaland.isReady()) && streaming) {
    streaming = false;
    closing = true;
    complete_capture = complete_capture && Deotaland.isReady();
    last_close_at = now - 20;
  }
  if (closing && now - last_close_at >= 20) {
    last_close_at = now;
    const esp_err_t result = Deotaland.closeStream(audio_stream, complete_capture);
    if (!retryable(result)) {
      closing = false;
      audio_stream = nullptr;
      // A permanent END error may leave the core's session open. Do not attach
      // a later StartCapture to it; a new connection resets the session.
      if (result != ESP_OK) {
        stream_fault = true;
        capture_requested = false;
        capture_duration = 0;
      }
      Serial.printf("closeStream(AUDIO): %s\n", esp_err_to_name(result));
    }
  }
  if (streaming && now - last_pcm_at >= 20) {
    last_pcm_at = now;
    int16_t pcm[320];  // 20 ms at 16 kHz
    makeTestPcm(pcm, 320);
    const esp_err_t result = Deotaland.writeStream(audio_stream, pcm, sizeof(pcm));
    // Dropped PCM is not a complete recording, even if capture later stops normally.
    if (retryable(result)) complete_capture = false;
    if (result != ESP_OK && result != ESP_ERR_NO_MEM && result != ESP_ERR_TIMEOUT) {
      Serial.printf("writeStream(AUDIO): %s\n", esp_err_to_name(result));
      streaming = false;
      capture_requested = false;
      closing = true;
      complete_capture = false;
      last_close_at = now - 20;
    }
  }
}
