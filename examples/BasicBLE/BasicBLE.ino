#include <Deotaland.h>

namespace {

void onState(agent_state_t state, void*) {
  const char* name = state == AGENT_STATE_READY ? "READY"
                     : state == AGENT_STATE_CONNECTED ? "CONNECTED"
                                                      : "DISCONNECTED";
  Serial.printf("Deotaland state: %s\n", name);
}

void onCustom(uint16_t command, const uint8_t*, size_t length, void*) {
  // This callback runs in the NimBLE host task. Queue lengthy work elsewhere.
  Serial.printf("Custom command 0x%02X (%u bytes)\n", command,
                static_cast<unsigned>(length));
}

agent_output_cb_t callbacks = {};
uint32_t last_battery_report = 0;

}  // namespace

void setup() {
  Serial.begin(115200);

  callbacks.on_custom = onCustom;

  agent_link_config_t config = {};
  config.device_name = "Deotaland-ESP32";
  config.caps = AGENT_CAP_BATTERY;
  config.output = &callbacks;
  config.on_state = onState;
  config.transport = AGENT_TRANSPORT_BLE;
  config.manufacturer = "Deotaland";
  config.model = "Arduino ESP32";
  config.firmware_rev = "0.2.0";

  const esp_err_t result = Deotaland.begin(config);
  if (result != ESP_OK) {
    Serial.printf("Deotaland.begin failed: %s\n", esp_err_to_name(result));
  }
}

void loop() {
  if (Deotaland.isReady() && millis() - last_battery_report >= 5000) {
    last_battery_report = millis();
    Deotaland.reportBattery(100, false);  // Replace with the board's real gauge.
  }
  delay(10);
}
