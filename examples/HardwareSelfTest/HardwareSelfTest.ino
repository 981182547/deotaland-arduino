/* ESP32 集成诊断：115200 串口输出 SELFTEST；部分检查需 App 连接和订阅。
 * 回调仅打印/计数，不驱动实际麦克风、屏幕或扬声器；不是外设自检驱动。
 * 本例含诊断日志，不作为实时音频产品的循环模板。
 */
#include <Deotaland.h>
#include <agent_link_transport.h>
#include <esp_heap_caps.h>

// Diagnostic sketch: callbacks print received commands and do not drive real
// speaker/display/haptic hardware. Serial output and heap diagnostics are not
// hard real-time operations; use short, queued callbacks in a product.

namespace {

int passed = 0;
int failed = 0;
bool link_tests_done = false;
bool ready_waiting = false;
uint32_t ready_since = 0;
bool connected_reported = false;
bool connected_tests_started = false;
bool startup_ok = false;
agent_stream_handle_t voice = nullptr;
bool voice_complete = false;
bool voice_closed = false;
uint8_t link_step = 0;
uint32_t step_started_at = 0;
uint32_t last_step_at = 0;

void check(const char* name, esp_err_t actual, esp_err_t expected) {
  const bool ok = actual == expected;
  Serial.printf("SELFTEST:%s:%s actual=%s(0x%X) expected=%s(0x%X)\n",
                ok ? "PASS" : "FAIL", name, esp_err_to_name(actual), actual,
                esp_err_to_name(expected), expected);
  ok ? ++passed : ++failed;
}

void checkBool(const char* name, bool ok) {
  Serial.printf("SELFTEST:%s:%s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}

void checkHeap(const char* stage) {
  checkBool(stage, heap_caps_check_integrity_all(true));
}

esp_err_t readingExpected(const char* id, size_t bytes) {
  const uint16_t mtu = agent_transport_ble_att_mtu();
  const size_t frame_budget = mtu >= 23 ? mtu - 3 : 20;
  return 8 + strlen(id) + bytes <= frame_budget ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void onState(agent_state_t state, void*) {
  Serial.printf("CALLBACK:state=%d\n", state);
  if (state != AGENT_STATE_READY) {
    ready_waiting = false;
    if (connected_tests_started && !link_tests_done) {
      checkBool("connection-lost-during-tests", false);
      link_tests_done = true;
      voice = nullptr;
    }
  }
}
void onText(const char* text, void*) { Serial.printf("CALLBACK:text=%s\n", text); }
void onHaptic(uint32_t ms, void*) {
  Serial.printf("CALLBACK:haptic=%lu\n", static_cast<unsigned long>(ms));
}
void onLed(uint32_t rgb, void*) {
  Serial.printf("CALLBACK:led=0x%08lX\n", static_cast<unsigned long>(rgb));
}
void onAudioEnd(void*) { Serial.println("CALLBACK:audio_end"); }
void onListen(bool start, uint32_t max_ms, void*) {
  Serial.printf("CALLBACK:listen=%d,max_ms=%lu\n", start,
                static_cast<unsigned long>(max_ms));
}
bool onCommand(uint16_t command, const uint8_t*, size_t,
               uint8_t* response, size_t capacity, size_t* response_length, void*) {
  if (command != 0x7E || capacity < 3) return false;
  response[0] = 'o'; response[1] = 'k'; response[2] = '!';
  *response_length = 3;
  Serial.println("CALLBACK:command=0x7E");
  return true;
}
void onActuate(const char* id, const uint8_t* args, size_t length, void*) {
  Serial.printf("CALLBACK:actuate=%s,len=%u,value=%u\n", id,
                static_cast<unsigned>(length), args && length ? args[0] : 0);
}

agent_output_cb_t callbacks = {};

const agent_link_io_desc_t endpoints[] = {
  {.id="bool0", .dir=AGENT_IO_IN, .kind="bool", .value=AGENT_VAL_BOOL},
  {.id="i32_0", .dir=AGENT_IO_IN, .kind="integer", .value=AGENT_VAL_I32},
  {.id="u16_0", .dir=AGENT_IO_IN, .kind="unsigned", .value=AGENT_VAL_U16},
  {.id="f32_0", .dir=AGENT_IO_IN, .kind="float", .value=AGENT_VAL_F32,
   .display_name="Float reading", .event=AGENT_EVT_ON_CHANGE},
  {.id="vec2_0", .dir=AGENT_IO_IN, .kind="vector2", .value=AGENT_VAL_VEC2},
  {.id="vec3_0", .dir=AGENT_IO_IN, .kind="vector3", .value=AGENT_VAL_VEC3},
  {.id="rgb0", .dir=AGENT_IO_IN, .kind="color", .value=AGENT_VAL_RGB},
  {.id="blob0", .dir=AGENT_IO_IN, .kind="blob", .value=AGENT_VAL_BLOB},
  {.id="str0", .dir=AGENT_IO_IN, .kind="text", .value=AGENT_VAL_STR,
   .audience=AGENT_AUD_USER},
  {.id="relay0", .dir=AGENT_IO_OUT, .kind="relay", .value=AGENT_VAL_BOOL,
   .args_schema="{\"type\":\"boolean\"}"},
};

const agent_link_io_desc_t runtime_endpoint = {
  .id="runtime0", .dir=AGENT_IO_IN, .kind="runtime", .value=AGENT_VAL_U16,
  .default_json="0", .event=AGENT_EVT_THRESHOLD,
};

void runConnectedTestStep(uint32_t now) {
  const uint8_t b = 1;
  const int32_t i32 = -123456;
  const uint16_t u16 = 54321;
  const float f32 = 12.5f;
  const float vec2[2] = {1.0f, -1.0f};
  const float vec3[3] = {1.0f, 2.0f, 3.0f};
  const uint32_t rgb = 0x00123456;
  const uint8_t blob[3] = {0xAA, 0x55, 0x01};
  const char text[] = "ESP32-S3";

  int16_t pcm[160] = {};
  agent_stream_handle_t unsupported = nullptr;
  agent_stream_opts_t options = {};
  options.name = "selftest";
  options.width = 1;
  options.height = 1;
  options.total_bytes = 2;

  const char* name = nullptr;
  esp_err_t actual = ESP_OK;
  esp_err_t expected = ESP_OK;
  // One operation per service step, with retries limited to five seconds.
  // These checks validate local acceptance, not successful delivery to the App.
  const char* ids[] = {"bool0", "i32_0", "u16_0", "f32_0", "vec2_0", "vec3_0", "rgb0", "blob0", "str0"};
  const void* values[] = {&b, &i32, &u16, &f32, vec2, vec3, &rgb, blob, text};
  const size_t sizes[] = {sizeof(b), sizeof(i32), sizeof(u16), sizeof(f32), sizeof(vec2), sizeof(vec3), sizeof(rgb), sizeof(blob), strlen(text)};
  if (link_step < 9) {
    name = ids[link_step];
    expected = readingExpected(name, sizes[link_step]);
    actual = Deotaland.pushReading(name, values[link_step], sizes[link_step]);
  } else switch (link_step) {
    case 9: name="battery"; actual=Deotaland.reportBattery(77, true); break;
    case 10: name="event-button"; actual=Deotaland.pushEvent(AGENT_EVT_BUTTON, &b, 1); break;
    case 11: name="event-sensor"; actual=Deotaland.pushEvent(AGENT_EVT_SENSOR, blob, sizeof(blob)); break;
    case 12: name="event-wakeword"; actual=Deotaland.pushEvent(AGENT_EVT_WAKEWORD); break;
    case 13: name="event-custom"; actual=Deotaland.pushEvent(AGENT_EVT_CUSTOM, blob, sizeof(blob)); break;
    case 14: name="stream-voice-open"; actual=Deotaland.openStream(AGENT_STREAM_VOICE, nullptr, &voice); break;
    case 15:
      if (voice) {
        name="stream-voice-write"; actual=Deotaland.writeStream(voice, pcm, sizeof(pcm));
        voice_complete = actual == ESP_OK;
      }
      break;
    case 16:
      if (voice) {
        name="stream-voice-close"; actual=Deotaland.closeStream(voice, voice_complete);
        voice_closed = actual == ESP_OK;
      }
      break;
    case 17:
      if (voice && voice_closed) { name="stream-close-idempotent"; actual=Deotaland.closeStream(voice); }
      break;
    case 18:
    case 19: {
      // The bundled host enables CoC independently of the Core's sdkconfig.
      // Probe the active backend rather than testing a sketch-local Core macro.
      const agent_stream_t kind = link_step == 18 ? AGENT_STREAM_AUDIO : AGENT_STREAM_IMAGE;
      agent_stream_status_t status = {};
      if (Deotaland.streamStatus(kind, &status) == ESP_OK) {
        Serial.println("SELFTEST:SKIP:CoC stream needs a peer session and separate hardware test");
      } else {
        name = link_step == 18 ? "stream-audio-backend-limit" : "stream-image-backend-limit";
        expected = ESP_ERR_NOT_SUPPORTED;
        actual = Deotaland.openStream(kind, &options, &unsupported);
      }
      break;
    }
    case 20: name="selected-agent-upstream-limit"; expected=ESP_ERR_NOT_SUPPORTED; actual=Deotaland.selectAgent("agent-1"); break;
    case 21: name="stream-video-limit"; expected=ESP_ERR_NOT_SUPPORTED; actual=Deotaland.openStream(AGENT_STREAM_VIDEO, &options, &unsupported); break;
    case 22: name="stream-file-limit"; expected=ESP_ERR_NOT_SUPPORTED; actual=Deotaland.openStream(AGENT_STREAM_FILE, &options, &unsupported); break;
    case 23: name="prompt"; actual=Deotaland.pushPrompt("Self-test"); break;
    case 24: name="runtime-register"; actual=Deotaland.registerIo(runtime_endpoint); break;
    case 25: name="manifest-notify"; actual=Deotaland.notifyManifestChanged(); break;
    case 26: name="runtime-reading"; actual=Deotaland.pushReading("runtime0", &u16, sizeof(u16)); break;
    default: link_tests_done=true; return;
  }
  if (name) {
    const bool retry = actual == ESP_ERR_NO_MEM || actual == ESP_ERR_TIMEOUT ||
                       (link_step == 14 && actual == ESP_ERR_INVALID_STATE);
    if (expected == ESP_OK && retry && now - step_started_at < 5000) return;
    check(name, actual, expected);
  } else if (link_step >= 15 && link_step <= 17) {
    Serial.println("SELFTEST:SKIP:voice operation requires the preceding operation to succeed");
  }
  if (link_step == 17) voice = nullptr;
  ++link_step;
  step_started_at = now;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("SELFTEST:START");
  checkHeap("heap-at-start");

  check("init-null", agent_link_init(nullptr), ESP_ERR_INVALID_ARG);
  check("register-null", agent_link_register_io(nullptr, nullptr, nullptr), ESP_ERR_INVALID_ARG);
  for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); ++i) {
    check("register", Deotaland.registerIo(endpoints[i],
          endpoints[i].dir == AGENT_IO_OUT ? onActuate : nullptr), ESP_OK);
    checkHeap("heap-after-register");
  }
  check("register-duplicate", Deotaland.registerIo(endpoints[0]), ESP_ERR_INVALID_STATE);

  callbacks.on_audio_end = onAudioEnd;
  callbacks.on_show_text = onText;
  callbacks.on_haptic = onHaptic;
  callbacks.on_led = onLed;
  callbacks.on_command = onCommand;
  callbacks.on_listen = onListen;

  agent_link_config_t config = {};
  config.device_name = "Deotaland-SelfTest";
  config.caps = AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN |
                AGENT_CAP_BUTTON | AGENT_CAP_HAPTIC | AGENT_CAP_BATTERY |
                AGENT_CAP_LED | AGENT_CAP_SENSOR | AGENT_CAP_ACTUATOR |
                AGENT_CAP_RECORDING | AGENT_CAP_CAMERA;
  config.output = &callbacks;
  config.on_state = onState;
  config.transport = AGENT_TRANSPORT_BLE;
  config.manufacturer = "Deotaland";
  config.model = "ESP32-S3 SelfTest";
  config.firmware_rev = "2.0.0";

  const esp_err_t begin_result = Deotaland.begin(config);
  startup_ok = begin_result == ESP_OK;
  check("begin", begin_result, ESP_OK);
  if (!startup_ok) {
    Serial.printf("SELFTEST:BOOT_DONE passed=%d failed=%d (start failed)\n", passed, failed);
    return;
  }
  check("begin-twice", Deotaland.begin(config), ESP_ERR_INVALID_STATE);
  checkBool("state-disconnected", Deotaland.state() == AGENT_STATE_DISCONNECTED);
  check("battery-range", Deotaland.reportBattery(101, false), ESP_ERR_INVALID_ARG);
  check("reading-null", Deotaland.pushReading("bool0", nullptr, 1), ESP_ERR_INVALID_ARG);
  check("reading-size", Deotaland.pushReading("bool0", &failed, sizeof(failed)),
        ESP_ERR_INVALID_STATE);  // link-state validation occurs before type-size validation
  check("event-null", Deotaland.pushEvent(AGENT_EVT_BUTTON, nullptr, 1), ESP_ERR_INVALID_ARG);
  agent_stream_handle_t stream = nullptr;
  check("stream-open-offline", Deotaland.openStream(AGENT_STREAM_VOICE, nullptr, &stream),
        ESP_ERR_INVALID_STATE);
  check("stream-kind-invalid", Deotaland.openStream(AGENT_STREAM_KIND_COUNT, nullptr, &stream),
        ESP_ERR_INVALID_ARG);
  check("stream-write-null", Deotaland.writeStream(nullptr, "x", 1), ESP_ERR_INVALID_ARG);
  check("stream-close-null", Deotaland.closeStream(nullptr), ESP_ERR_INVALID_ARG);
  check("stream-send-null", Deotaland.sendStream(AGENT_STREAM_VOICE, nullptr, nullptr, 1),
        ESP_ERR_INVALID_ARG);
  check("manifest-offline", Deotaland.notifyManifestChanged(), ESP_OK);
  Serial.printf("SELFTEST:BOOT_DONE passed=%d failed=%d\n", passed, failed);
}

void loop() {
  if (!startup_ok) return;
  Deotaland.poll();
  const uint32_t now = millis();
  if (!Deotaland.isReady()) ready_waiting = false;
  if (Deotaland.isReady() && !link_tests_done) {
    if (!ready_waiting) { ready_waiting = true; ready_since = now; }
    if (!connected_tests_started && now - ready_since >= 500) {
      connected_tests_started = true;
      step_started_at = last_step_at = now;
    }
    if (connected_tests_started && now - last_step_at >= 100) {
      last_step_at = now;
      runConnectedTestStep(now);
    }
  }
  if (link_tests_done && !connected_reported) {
    connected_reported = true;
    Serial.printf("SELFTEST:CONNECTED_DONE passed=%d failed=%d\n", passed, failed);
  }
}
