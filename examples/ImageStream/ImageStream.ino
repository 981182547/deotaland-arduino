/* 图片分块练习：发送 2x2 测试图，无相机采集。需 App 打开 IMAGE CoC 通道。
 * 普通 Core 未启用 CoC 时返回 NOT_SUPPORTED，安装其他 BLE 库不能补上该能力。
 * 写失败时保留原块和偏移，下轮重试；ESP_OK 仅表示本地接受。
 */
#include <Deotaland.h>

// 2x2 big-endian RGB565, no camera driver. For a camera JPEG, also set
// opts.encoding = AGENT_ENC_JPEG and use its real dimensions/byte count.
const uint8_t pixels[] = {0xf8,0x00, 0x07,0xe0, 0x00,0x1f, 0xff,0xff};
agent_stream_handle_t stream = nullptr;
enum Phase { Open, Write, Close, Abort, Done };
Phase phase = Open;
uint32_t lastAttempt = 0;
bool started = false;
esp_err_t startError = ESP_OK, supportError = ESP_ERR_INVALID_STATE;
uint32_t lastDiagnostic = 0;
bool diagnosticPending = true;

void printDiagnostic() {
  Serial.println("ImageStream：发送内置 2x2 测试图，无相机；每 10 秒显示诊断，? 可重看。");
  if (startError != ESP_OK) {
    Serial.print("启动失败: "); Serial.println(Deotaland.errorName(startError));
    Serial.println(Deotaland.errorHint(startError));
  } else if (supportError == ESP_ERR_NOT_SUPPORTED) {
    Serial.println("当前 Core 未提供 IMAGE/CoC，已停止图片任务。需使用启用 NimBLE CoC 的 Core；草图加宏或重连不能补齐。");
  } else if (supportError != ESP_OK) {
    Serial.print("IMAGE 能力检查失败: "); Serial.println(Deotaland.errorName(supportError));
    Serial.println(Deotaland.errorHint(supportError));
  } else {
    Serial.print("控制状态: "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.println("Core 支持 IMAGE；传图仍需 App 打开 IMAGE CoC 通道，ESP_OK 仅代表本地接受。");
  }
}

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    // Handles belong to one connection, including reconnects inside one poll().
    phase = Open;
    stream = nullptr;
  }
}

void setup() {
  Serial.begin(115200);
  startError = Deotaland.begin("Image-Sample", AGENT_CAP_CAMERA, nullptr, onState);
  started = startError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    // 在连接 App 前检查编入的功能；ESP_OK 不代表 CoC 已连接，不能检查 state 来推断能力。
    supportError = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
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
  if (!started || supportError != ESP_OK || !Deotaland.isReady() || phase == Done) return;
  if (uint32_t(now - lastAttempt) < 100) return;
  lastAttempt = now;
  esp_err_t result;
  if (phase == Open) {
    agent_stream_opts_t opts = {};
    opts.width = 2; opts.height = 2; opts.total_bytes = sizeof(pixels);
    result = Deotaland.openStream(AGENT_STREAM_IMAGE, &opts, &stream);
  } else if (phase == Write) result = Deotaland.writeStream(stream, pixels, sizeof(pixels));
  else result = Deotaland.closeStream(stream, phase != Abort);

  // Queue pressure: retry this operation without skipping/duplicating image data.
  if (result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT) return;
  // READY is the control channel; wait for the peer's separate IMAGE CoC session.
  if (phase == Open && result == ESP_ERR_INVALID_STATE) return;
  if (result == ESP_OK) {
    if (phase == Open) phase = Write;
    else if (phase == Write) phase = Close;
    else { phase = Done; stream = nullptr; }
  } else {
    Serial.printf("image transfer: %s\n", esp_err_to_name(result));
    // Permanent payload errors abort once; unsupported cores wait for reconnect.
    // If END itself fails permanently, leave the handle unused until disconnect.
    if (phase == Write) phase = Abort;
    else { phase = Done; stream = nullptr; }
  }
}
