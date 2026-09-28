/*
 * 第 1 课：只用 USB 验证 BLE 连接，不需要外接硬件。
 * 上传后打开 115200 串口，在兼容 Agent Link 的 App 内连接 Deotaland-ESP32。
 * READY = App 已订阅事件通知。扫描工具只连接、未订阅 0xFFC4 时是 CONNECTED。
 * 本例 80% 是模拟电量，不是电池测量；相同值由库去重。
 * 串口发送 ? 查看状态。详见 docs/BEGINNER-GUIDE.zh-CN.md。
 */
#include <Deotaland.h>

namespace {
bool started = false;
esp_err_t startError = ESP_OK;
bool stateChanged = true;
uint32_t lastReport = 0, lastAttempt = 0, lastStatus = 0;
uint32_t commandCount = 0;

void onState(agent_state_t, void*) {
  stateChanged = true; // 回调只记录状态；日志和耗时动作放到 loop()。
}

bool onCommand(uint16_t command, const uint8_t*, size_t,
               uint8_t*, size_t, size_t* responseLength, void*) {
  if (command != 0x7E) return false; // 其他命令交给库拒绝。
  ++commandCount;
  *responseLength = 0;
  return true;
}

void printError(const char* operation, esp_err_t error) {
  Serial.print(operation); Serial.print(": ");
  Serial.println(Deotaland.errorName(error));
  Serial.println(Deotaland.errorHint(error));
}

void printStatus() {
  if (!started) {
    printError("begin", startError);
    Serial.println("启动失败：按错误码检查开发板/Core 和配置，再复位。");
    return;
  }
  Serial.print("连接状态: "); Serial.print(Deotaland.stateName(Deotaland.state()));
  Serial.print(" | 自定义命令次数: "); Serial.println(commandCount);
  const char* id = Deotaland.deviceId();
  if (id) { Serial.print("设备 UUID: "); Serial.println(id); }
  if (Deotaland.isReady()) Serial.println("事件通道可用。模拟电量会在本地排队发送。");
  else Serial.println("在 App 内连接 Deotaland-ESP32，并订阅事件通知 0xFFC4。");
}
} // namespace

void setup() {
  Serial.begin(115200);
  // {} 清零未使用字段，避免未初始化的指针和选项。
  agent_output_cb_t callbacks = {};
  callbacks.on_command = onCommand;
  agent_link_config_t config = {};
  config.device_name = "Deotaland-ESP32"; // 可修改广播名称。
  config.model = "Arduino ESP32";
  config.firmware_rev = "1.0.0"; // 你的应用版本，与库版本无关。
  config.caps = AGENT_CAP_BATTERY;
  config.output = &callbacks; // begin 会复制此回调表。
  config.on_state = onState;
  config.transport = AGENT_TRANSPORT_BLE;
  startError = Deotaland.begin(config);
  started = startError == ESP_OK;
  Serial.println("BasicBLE：串口发送 ? 查看状态；模拟电量为 80%。");
}

void loop() {
  if (started) Deotaland.poll(); // 每轮调用，派发回调并推进协议。
  const uint32_t now = millis();
  // 限制每轮输入数量，不使用阻塞的 readString()/长 delay()。
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n)
    if (Serial.read() == '?') stateChanged = true;
  if (stateChanged || uint32_t(now - lastStatus) >= 10000) {
    stateChanged = false;
    lastStatus = now;
    printStatus(); // 低频日志；稍后打开串口也能看到状态。
  }
  if (!started || !Deotaland.isReady()) return;
  // 无符号时间差可跨 millis() 回绕；拥塞最多每 100ms 重试一次。
  if (uint32_t(now - lastReport) >= 5000 && uint32_t(now - lastAttempt) >= 100) {
    lastAttempt = now;
    const esp_err_t result = Deotaland.reportBattery(80, false);
    if (result == ESP_OK) lastReport = now;
    else if (!Deotaland.isRetryable(result)) {
      lastReport = now; // 永久错误也限频，避免刷屏拖慢通信。
      printError("reportBattery", result);
    }
  }
}
