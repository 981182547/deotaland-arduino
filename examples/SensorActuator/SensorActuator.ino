#include <Deotaland.h>

namespace {

constexpr uint8_t kLedPin = 2;  // Change to the LED pin on your board.

const agent_link_io_desc_t uptime_endpoint = {
  .id = "uptime0",
  .dir = AGENT_IO_IN,
  .kind = "uptime",
  .value = AGENT_VAL_I32,
  .unit = "ms",
  .desc = "device uptime",
  .range_min = 0,
  .range_max = 2147483647.0f,
  .rate_hz = 1,
  .args_schema = nullptr,
};

const agent_link_io_desc_t led_endpoint = {
  .id = "led0",
  .dir = AGENT_IO_OUT,
  .kind = "led",
  .value = AGENT_VAL_BOOL,
  .unit = nullptr,
  .desc = "on-board status LED",
  .range_min = 0,
  .range_max = 1,
  .rate_hz = 0,
  .args_schema = "{\"type\":\"boolean\"}",
};

void actuate(const char* id, const uint8_t* args, size_t length, void*) {
  // AGENT_VAL_BOOL is one raw byte in the current protocol.
  if (strcmp(id, "led0") == 0 && args && length >= 1) {
    digitalWrite(kLedPin, args[0] ? HIGH : LOW);
  }
}

uint32_t last_reading = 0;

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kLedPin, OUTPUT);

  // Endpoint descriptors must have static lifetime and must be registered first.
  ESP_ERROR_CHECK(Deotaland.registerIo(uptime_endpoint));
  ESP_ERROR_CHECK(Deotaland.registerIo(led_endpoint, actuate));

  const esp_err_t result = Deotaland.begin(
      "Deotaland-IO", AGENT_CAP_SENSOR | AGENT_CAP_ACTUATOR);
  if (result != ESP_OK) {
    Serial.printf("Deotaland.begin failed: %s\n", esp_err_to_name(result));
  }
}

void loop() {
  if (Deotaland.isReady() && millis() - last_reading >= 1000) {
    last_reading = millis();
    const int32_t uptime_ms = static_cast<int32_t>(millis());
    Deotaland.pushReading("uptime0", &uptime_ms, sizeof(uptime_ms));
  }
  delay(10);
}

