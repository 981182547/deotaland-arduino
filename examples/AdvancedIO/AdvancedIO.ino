/* 进阶 I/O：多个模拟读数、字符串执行器、动态注册和手动发送重试。
 * 新手先看 CachedSensor 的 updateFloat，再学习这里的 pushReading。
 * 温度/启动次数为演示值；STR 回调是带长度的字节，不保证 NUL 结尾。
 */
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
  .unit = "count", .desc = "demo constant, not a persisted boot counter", .display_name = "Boot count",
  .audience = AGENT_AUD_USER,
};

const char* current_mode = "normal";
void actuate(const char* id, const uint8_t* args, size_t length, void*) {
  // STR values are raw UTF-8 bytes, not JSON strings and not NUL-terminated.
  if (strcmp(id, "mode0") != 0 || !args) return;
  const char* choices[] = {"quiet", "normal", "boost"};
  for (const char* choice : choices) {
    if (length == strlen(choice) && memcmp(args, choice, length) == 0) {
      current_mode = choice;
      // Apply a board-specific mode asynchronously here. This example only
      // changes the reported status; it has no motor/fan/other hardware driver.
      return;
    }
  }
}

uint32_t last_report = 0;
uint32_t last_attempt = 0;
uint32_t setup_at = 0;
uint8_t report_step = 0;
float pending_temperature = 0;
const char* pending_status = "normal";
bool report_pending = false;
bool started = false;
bool runtime_added = false;
bool runtime_attempted = false;

}  // namespace

void setup() {
  Serial.begin(115200);
  setup_at = millis();
  esp_err_t result = Deotaland.registerIo(temperature);
  if (result == ESP_OK) result = Deotaland.registerIo(status_text);
  if (result == ESP_OK) result = Deotaland.registerIo(mode, actuate);
  if (result != ESP_OK) {
    Serial.printf("registerIo: %s\n", esp_err_to_name(result));
    return;
  }

  agent_link_config_t config = {};
  config.device_name = "Deotaland-AdvancedIO";
  config.caps = AGENT_CAP_SENSOR | AGENT_CAP_ACTUATOR;
  config.firmware_rev = "2.0.0";
  result = Deotaland.begin(config);
  started = result == ESP_OK;
  Serial.printf("begin: %s\n", esp_err_to_name(result));
}

void loop() {
  if (!started) return;
  Deotaland.poll();
  const uint32_t now = millis();
  // The new API permits endpoints to be added after startup. Descriptors must have static lifetime.
  if (!runtime_attempted && uint32_t(now - setup_at) >= 10000) {
    runtime_attempted = true;
    const esp_err_t result = Deotaland.registerIo(boot_count);
    runtime_added = result == ESP_OK;
    if (runtime_added) {
      // registerIo 已自动安排发送新清单，无需再次 notifyManifestChanged。
      Serial.println("manifest scheduled after registration");
    } else Serial.printf("runtime registerIo: %s\n", esp_err_to_name(result));
  }

  if (!Deotaland.isReady()) { report_pending = false; return; }
  if (!report_pending && uint32_t(now - last_report) >= 1000) {
    report_pending = true;
    report_step = 0;
    // Synthetic temperature and boot count: replace with real/persisted values.
    pending_temperature = 20.0f + static_cast<float>((now / 1000) % 10) * 0.1f;
    pending_status = current_mode;
  }
  if (!report_pending || uint32_t(now - last_attempt) < 100) return;
  last_attempt = now;
  esp_err_t result;
  if (report_step == 0) result = Deotaland.pushReading("temp0", &pending_temperature, sizeof(pending_temperature));
  else if (report_step == 1) result = Deotaland.pushReading("status0", pending_status, strlen(pending_status));
  else {
    const int32_t count = 1;
    result = Deotaland.pushReading("boots0", &count, sizeof(count));
  }
  // One endpoint per iteration; preserve the current value when the queue is full.
  if (result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT) return;
  if (result != ESP_OK) Serial.printf("pushReading: %s\n", esp_err_to_name(result));
  if (++report_step >= (runtime_added ? 3 : 2)) {
    report_pending = false;
    last_report = now;
  }
}
