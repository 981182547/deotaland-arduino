/*
 * 第 3 课：上报运行时间 + 接收开关命令，默认无需接线。先运行 BasicBLE。
 * 在兼容 App 内连接 Deotaland-IO，将 led0 设为 true/false，观察串口。
 * uptime0 是秒数，enabled0 是当前软件开关状态，led0 是控制端点。
 * kLedPin=-1 只改变软件状态；接入普通 GPIO LED 后才填真实引脚和极性。
 * 地址灯不适用 digitalWrite；不要照搬其他型号的 LED_BUILTIN 或 GPIO2。
 */
#include <Deotaland.h>

namespace {
constexpr int kLedPin = -1; // 修改点 1：普通 LED 引脚；-1 = 纯软件练习。
constexpr bool kLedActiveLow = false; // 修改点 2：低电平点亮时改为 true。
bool started = false, enabled = false, applied = false, targetChanged = true;
uint32_t lastSample = 0, lastTick = 0;
uint64_t uptimeMs = 0;
// 描述符必须持续有效：不能用 setup() 内的普通局部变量。
agent_link_io_desc_t uptimeEndpoint = {}, stateEndpoint = {}, ledEndpoint = {};

void actuate(const char*, const uint8_t* args, size_t length, void*) {
  // BOOL 是原始 0/1 字节，不是字符串 "true"；库也会做基础校验。
  if (!args || length != 1 || args[0] > 1) return;
  enabled = args[0] != 0; // 设为明确值；重复命令不会意外翻转状态。
  targetChanged = true; // 回调不等待，loop() 再应用硬件状态。
}

void check(const char* operation, esp_err_t result) {
  if (result == ESP_OK) return;
  Serial.print(operation); Serial.print(": ");
  Serial.println(Deotaland.errorName(result)); Serial.println(Deotaland.errorHint(result));
}
} // namespace

void setup() {
  Serial.begin(115200);
  if (kLedPin >= 0) {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, kLedActiveLow ? HIGH : LOW);
  }
  lastTick = millis();
  uptimeMs = lastTick;
  uptimeEndpoint.id = "uptime0";
  uptimeEndpoint.dir = AGENT_IO_IN; // IN：从硬件输入，发送给 Agent。
  uptimeEndpoint.kind = "uptime";
  uptimeEndpoint.value = AGENT_VAL_I32;
  uptimeEndpoint.unit = "s";
  uptimeEndpoint.desc = "device uptime in seconds";
  uptimeEndpoint.rate_hz = 1; // 频率描述；真实采样仍由 loop 负责。
  stateEndpoint.id = "enabled0";
  stateEndpoint.dir = AGENT_IO_IN;
  stateEndpoint.kind = "switch.state";
  stateEndpoint.value = AGENT_VAL_BOOL;
  stateEndpoint.desc = "current demo switch state";
  ledEndpoint.id = "led0";
  ledEndpoint.dir = AGENT_IO_OUT; // OUT：Agent 控制硬件输出。
  ledEndpoint.kind = "led";
  ledEndpoint.value = AGENT_VAL_BOOL;
  ledEndpoint.desc = "set demo switch on or off; GPIO optional";
  ledEndpoint.args_schema = "{\"type\":\"boolean\"}";

  esp_err_t result = Deotaland.registerIo(uptimeEndpoint);
  if (result == ESP_OK) result = Deotaland.registerIo(stateEndpoint);
  if (result == ESP_OK) result = Deotaland.registerIo(ledEndpoint, actuate);
  if (result != ESP_OK) { check("registerIo", result); return; }
  agent_link_config_t config = {};
  config.device_name = "Deotaland-IO";
  config.firmware_rev = "1.0.0";
  config.caps = AGENT_CAP_SENSOR | AGENT_CAP_ACTUATOR;
  result = Deotaland.begin(config);
  started = result == ESP_OK;
  check("begin", result);
  if (started) Serial.println("在 App 内连接 Deotaland-IO，设置 led0=true/false。默认不操作 GPIO。");
}

void loop() {
  if (!started) return;
  Deotaland.poll();
  const uint32_t now = millis();
  uptimeMs += uint32_t(now - lastTick); // 累加差值，跨 millis() 回绕。
  lastTick = now;
  if (targetChanged) {
    targetChanged = false;
    if (kLedPin >= 0 && applied != enabled)
      digitalWrite(kLedPin, (enabled != kLedActiveLow) ? HIGH : LOW);
    applied = enabled;
    check("updateBool", Deotaland.updateBool("enabled0", enabled));
    Serial.print("开关目标: "); Serial.println(enabled ? "ON" : "OFF");
  }
  if (uint32_t(now - lastSample) >= 1000) {
    lastSample = now;
    const uint64_t seconds = uptimeMs / 1000;
    const int32_t value = seconds > INT32_MAX ? INT32_MAX : static_cast<int32_t>(seconds);
    // 不用 &value/sizeof；离线也能更新。poll 会尝试发送最新值。
    check("updateInt32", Deotaland.updateInt32("uptime0", value));
  }
  // updateXXX 的 ESP_OK = 缓存成功，不代表 App 已收到，无需手写发送重试。
}
