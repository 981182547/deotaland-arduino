/*
 * 第 2 课：每秒更新一个模拟温度，无需接线。
 * 在 App 内连接 Deotaland-Sensor，查看 temp0。串口设为 115200。
 * 断线时照常采样；重连只发送最新值。此接口不保存完整历史记录。
 * 接真实传感器时替换 readTemperatureC()；驱动应快速返回，勿在回调里等待。
 */
#include <Deotaland.h>

namespace {
agent_link_io_desc_t temperature = {}; // 全局：描述符及字符串须持续有效。
bool started = false;
uint32_t lastSample = 0, lastLog = 0;
float latest = 0;
esp_err_t sampleError = ESP_OK;

float readTemperatureC() {
  // 模拟值每 10 秒循环：20.0、20.1 ... 20.9 摄氏度，不代表实际环境。
  return 20.0f + static_cast<float>((millis() / 1000) % 10) * 0.1f;
}
} // namespace

void setup() {
  Serial.begin(115200);
  temperature.id = "temp0";       // 短且唯一；updateFloat 使用相同 ID。
  temperature.dir = AGENT_IO_IN;
  temperature.kind = "temperature";
  temperature.value = AGENT_VAL_F32; // 必须与 updateFloat 匹配。
  temperature.unit = "C";
  temperature.desc = "simulated temperature; replace with a real sensor";
  temperature.display_name = "Demo temperature";
  temperature.rate_hz = 1;        // 只是描述，实际采样由 loop 调度。
  esp_err_t result = Deotaland.registerIo(temperature);
  if (result == ESP_OK) {
    agent_link_config_t config = {};
    config.device_name = "Deotaland-Sensor";
    config.firmware_rev = "1.0.0";
    config.caps = AGENT_CAP_SENSOR;
    result = Deotaland.begin(config);
  }
  started = result == ESP_OK;
  Serial.println(Deotaland.errorName(result));
  if (!started) Serial.println(Deotaland.errorHint(result));
}

void loop() {
  if (!started) return;
  Deotaland.poll();
  const uint32_t now = millis();
  if (uint32_t(now - lastSample) >= 1000) {
    lastSample = now;
    latest = readTemperatureC();
    // 一行即可：复制数据、校验 F32、缓存。NaN/Inf 被拒绝且保留旧缓存。
    sampleError = Deotaland.updateFloat("temp0", latest);
  }
  if (uint32_t(now - lastLog) >= 5000) {
    lastLog = now; // 少打印，避免串口占满拖慢 poll。
    Serial.print("模拟温度 C: "); Serial.print(latest, 1);
    Serial.print(" | "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.print("本次缓存: "); Serial.println(Deotaland.errorName(sampleError));
    // last_error 是历史上最近一次错误，成功不会清零，不能当作当前连接状态。
    const auto stats = Deotaland.stats();
    if (stats.last_error != ESP_OK) {
      Serial.print("最近一次后台错误: "); Serial.println(Deotaland.errorName(stats.last_error));
      Serial.println(Deotaland.errorHint(stats.last_error));
    }
  }
}
