/*
 * 第 4 课：串口发送 p，向 Agent 发送“你好”。不需要物理按键。
 * 在兼容 App 内连接 Deotaland-Prompt，再从 115200 串口发送 p（可带换行）。
 * ? = 查看状态。一次排一个提示词，断线或超过 5 秒取消，避免重连误发。
 * UTF-8 按字节计数：这里 6 字节能放入最小 BLE 包。
 * ESP_OK 仅代表本地接受；Agent 回复还取决于 App/账号/服务端。
 */
#include <Deotaland.h>
#include <string.h>

namespace {
const char kPrompt[] = "你好"; // 修改这里；一个提示词必须放入一帧，不能随意拆句。
bool started = false, pending = false, cancelled = false;
uint32_t queuedAt = 0, lastAttempt = 0;

void onState(agent_state_t state, void*) {
  // 即使断开、重连发生在同一轮 poll，也取消旧会话的待发提示词。
  if (state != AGENT_STATE_READY && pending) { pending = false; cancelled = true; }
}

void printStatus() {
  Serial.print("状态: "); Serial.print(Deotaland.stateName(Deotaland.state()));
  Serial.print("，单条提示词最多字节: "); Serial.println(Deotaland.maxPromptBytes());
  Serial.println("p = 发送提示词；? = 查看状态。需先在 App 内连接。");
}
} // namespace

void setup() {
  Serial.begin(115200);
  agent_link_config_t config = {};
  config.device_name = "Deotaland-Prompt";
  config.firmware_rev = "1.0.0";
  config.on_state = onState;
  const esp_err_t result = Deotaland.begin(config);
  started = result == ESP_OK;
  Serial.println(Deotaland.errorName(result));
  if (!started) Serial.println(Deotaland.errorHint(result));
  printStatus();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  if (cancelled) { cancelled = false; Serial.println("连接变化：已取消待发提示词。"); }
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n) {
    const char key = static_cast<char>(Serial.read());
    if (key == '?') printStatus();
    if (key != 'p' && key != 'P') continue; // 忽略换行符。
    if (!started || !Deotaland.isReady()) { Serial.println("尚未 READY，请先连接 App。"); continue; }
    if (pending) { Serial.println("已有待发提示词，请稍等。"); continue; }
    if (strlen(kPrompt) > Deotaland.maxPromptBytes()) {
      Serial.println("提示词过长：缩短文本，或让 App 协商更大的 MTU 后再试。"); continue;
    }
    pending = true;
    queuedAt = now;
    lastAttempt = now - 100; // 允许立即尝试一次。
  }
  if (!pending) return;
  if (uint32_t(now - queuedAt) >= 5000) {
    pending = false; Serial.println("等待超过 5 秒，已取消。可再次发送 p。"); return;
  }
  if (uint32_t(now - lastAttempt) < 100) return;
  lastAttempt = now;
  const esp_err_t result = Deotaland.pushPrompt(kPrompt);
  if (Deotaland.isRetryable(result)) return; // 后续 loop 再试，保持 poll 运转。
  pending = false;
  if (result == ESP_OK) Serial.println("提示词已进入本地发送队列，请在 App 查看结果。");
  else { Serial.println(Deotaland.errorName(result)); Serial.println(Deotaland.errorHint(result)); }
}
