/* 生命周期诊断：自动验证启动、停止、等待 canBegin 和再次启动；串口 115200。
 * 适合排查重复 begin/后台驱动未收尾的问题，入门应用先用 BasicBLE。
 */
#include <Deotaland.h>

namespace {

agent_link_config_t config = {};
enum Phase { WaitFirstStart, Running, WaitRestart, Done };
Phase phase = WaitFirstStart;
uint32_t phase_started_at = 0;
uint32_t last_attempt_at = 0;

void printResult(const char* name, esp_err_t result) {
  Serial.printf("LIFECYCLE:%s:%s (%s)\n", result == ESP_OK ? "PASS" : "FAIL",
                name, esp_err_to_name(result));
}

void onState(agent_state_t state, void*) {
  Serial.printf("LIFECYCLE:STATE:%d\n", static_cast<int>(state));
}

}  // namespace

void setup() {
  Serial.begin(115200);

  config.device_name = "Deotaland-Lifecycle";
  config.caps = AGENT_CAP_BATTERY;
  config.on_state = onState;
  config.transport = AGENT_TRANSPORT_BLE;
  config.firmware_rev = "2.0.0";
  phase_started_at = millis();
}

void loop() {
  Deotaland.poll();
  const uint32_t now = millis();
  if (phase == WaitFirstStart && now - phase_started_at >= 1000) {
    const esp_err_t result = Deotaland.begin(config);
    printResult("first-begin", result);
    if (result != ESP_OK) {
      Serial.println("LIFECYCLE:DONE:first-start-failed");
      phase = Done;
      return;
    }
    phase = Running;
    phase_started_at = now;
  } else if (phase == Running && now - phase_started_at >= 2000) {
    Deotaland.end();
    Serial.printf("LIFECYCLE:%s:first-end state=%d\n",
                  Deotaland.state() == AGENT_STATE_DISCONNECTED ? "PASS" : "FAIL",
                  static_cast<int>(Deotaland.state()));
    Deotaland.end();
    Serial.printf("LIFECYCLE:%s:idempotent-end\n",
                  Deotaland.state() == AGENT_STATE_DISCONNECTED ? "PASS" : "FAIL");
    phase = WaitRestart;
    phase_started_at = last_attempt_at = now;
  } else if (phase == WaitRestart && now - phase_started_at >= 500 &&
             now - last_attempt_at >= 100) {
    last_attempt_at = now;
    const esp_err_t result = Deotaland.begin(config);
    // BLE shutdown completes asynchronously; retry on a later loop iteration.
    if (result == ESP_ERR_INVALID_STATE && now - phase_started_at < 5000) return;
    printResult("second-begin", result);
    Serial.println("LIFECYCLE:DONE");
    phase = Done;
  }
}
