#include <Deotaland.h>

namespace {

const agent_link_io_desc_t temperature = {
  .id = "temp0", .dir = AGENT_IO_IN, .kind = "temperature", .value = AGENT_VAL_F32,
  .unit = "C", .desc = "ambient temperature", .range_min = -40, .range_max = 125,
  .rate_hz = 1, .display_name = "Temperature", .event = AGENT_EVT_ON_CHANGE,
};

const agent_link_io_desc_t status_text = {
  .id = "status0", .dir = AGENT_IO_IN, .kind = "status.text", .value = AGENT_VAL_STR,
  .desc = "human-readable device status", .display_name = "Status",
};

const agent_link_io_desc_t mode = {
  .id = "mode0", .dir = AGENT_IO_OUT, .kind = "mode", .value = AGENT_VAL_STR,
  .desc = "device operating mode", .args_schema = "{\"type\":\"string\"}",
  .display_name = "Operating mode", .enum_json = "[\"quiet\",\"normal\",\"boost\"]",
  .default_json = "\"normal\"",
};

const agent_link_io_desc_t boot_count = {
  .id = "boots0", .dir = AGENT_IO_IN, .kind = "counter", .value = AGENT_VAL_I32,
  .unit = "count", .desc = "runtime-added example endpoint", .display_name = "Boot count",
  .audience = AGENT_AUD_USER,
};

void actuate(const char* id, const uint8_t* args, size_t length, void*) {
  Serial.printf("actuate %s: %.*s\n", id, static_cast<int>(length),
                args ? reinterpret_cast<const char*>(args) : "");
}

uint32_t last_report = 0;
bool runtime_added = false;

}  // namespace

void setup() {
  Serial.begin(115200);
  Deotaland.registerIo(temperature);
  Deotaland.registerIo(status_text);
  Deotaland.registerIo(mode, actuate);

  const esp_err_t result = Deotaland.begin(
      "Deotaland-AdvancedIO", AGENT_CAP_SENSOR | AGENT_CAP_ACTUATOR);
  Serial.printf("begin: %s\n", esp_err_to_name(result));
}

void loop() {
  // The new API permits endpoints to be added after startup. Descriptors must have static lifetime.
  if (!runtime_added && millis() > 10000) {
    runtime_added = true;
    if (Deotaland.registerIo(boot_count) == ESP_OK) {
      Serial.printf("manifest changed: %s\n",
                    esp_err_to_name(Deotaland.notifyManifestChanged()));
    }
  }

  if (Deotaland.isReady() && millis() - last_report >= 1000) {
    last_report = millis();
    const float celsius = 20.0f + static_cast<float>((millis() / 1000) % 10) * 0.1f;
    const char* status = "running";
    Deotaland.pushReading("temp0", &celsius, sizeof(celsius));
    Deotaland.pushReading("status0", status, strlen(status));
    if (runtime_added) {
      const int32_t count = 1;
      Deotaland.pushReading("boots0", &count, sizeof(count));
    }
  }
  delay(10);
}
