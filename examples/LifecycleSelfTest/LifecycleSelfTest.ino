#include <Deotaland.h>

namespace {

agent_link_config_t config = {};

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
  delay(1000);

  config.device_name = "Deotaland-Lifecycle";
  config.caps = AGENT_CAP_BATTERY;
  config.on_state = onState;
  config.transport = AGENT_TRANSPORT_BLE;

  printResult("first-begin", Deotaland.begin(config));
  delay(2000);
  Deotaland.end();
  Serial.printf("LIFECYCLE:%s:first-end state=%d\n",
                Deotaland.state() == AGENT_STATE_DISCONNECTED ? "PASS" : "FAIL",
                static_cast<int>(Deotaland.state()));

  Deotaland.end();
  Serial.println("LIFECYCLE:PASS:idempotent-end");
  delay(500);

  printResult("second-begin", Deotaland.begin(config));
  Serial.println("LIFECYCLE:DONE");
}

void loop() { delay(1000); }
