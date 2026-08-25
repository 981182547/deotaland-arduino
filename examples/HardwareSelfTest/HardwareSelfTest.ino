#include <Deotaland.h>
#include <esp_heap_caps.h>

namespace {

int passed = 0;
int failed = 0;
bool link_tests_done = false;

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

void onState(agent_state_t state, void*) {
  Serial.printf("CALLBACK:state=%d\n", state);
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
void onCustom(uint16_t command, const uint8_t*, size_t length, void*) {
  Serial.printf("CALLBACK:custom=0x%02X,len=%u\n", command,
                static_cast<unsigned>(length));
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

void runConnectedTests() {
  const uint8_t b = 1;
  const int32_t i32 = -123456;
  const uint16_t u16 = 54321;
  const float f32 = 12.5f;
  const float vec2[2] = {1.0f, -1.0f};
  const float vec3[3] = {1.0f, 2.0f, 3.0f};
  const uint32_t rgb = 0x00123456;
  const uint8_t blob[3] = {0xAA, 0x55, 0x01};
  const char text[] = "ESP32-S3";

  check("reading-bool", Deotaland.pushReading("bool0", &b, sizeof(b)), ESP_OK);
  check("reading-i32", Deotaland.pushReading("i32_0", &i32, sizeof(i32)), ESP_OK);
  check("reading-u16", Deotaland.pushReading("u16_0", &u16, sizeof(u16)), ESP_OK);
  check("reading-f32", Deotaland.pushReading("f32_0", &f32, sizeof(f32)), ESP_OK);
  check("reading-vec2", Deotaland.pushReading("vec2_0", vec2, sizeof(vec2)), ESP_OK);
  check("reading-vec3", Deotaland.pushReading("vec3_0", vec3, sizeof(vec3)), ESP_OK);
  check("reading-rgb", Deotaland.pushReading("rgb0", &rgb, sizeof(rgb)), ESP_OK);
  check("reading-blob", Deotaland.pushReading("blob0", blob, sizeof(blob)), ESP_OK);
  check("reading-str", Deotaland.pushReading("str0", text, strlen(text)), ESP_OK);
  check("battery", Deotaland.reportBattery(77, true), ESP_OK);
  check("event-button", Deotaland.pushEvent(AGENT_EVT_BUTTON, &b, 1), ESP_OK);
  check("event-sensor", Deotaland.pushEvent(AGENT_EVT_SENSOR, blob, sizeof(blob)), ESP_OK);
  check("event-wakeword", Deotaland.pushEvent(AGENT_EVT_WAKEWORD), ESP_OK);
  check("event-custom", Deotaland.pushEvent(AGENT_EVT_CUSTOM, blob, sizeof(blob)), ESP_OK);

  int16_t pcm[160] = {};
  check("voice-push", Deotaland.pushVoice(pcm, 160), ESP_OK);
  check("voice-end", Deotaland.endVoice(), ESP_OK);

#if defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0
  Serial.println("SELFTEST:INFO:L2CAP-enabled; ASR requires peer CoC session");
#else
  check("asr-core-limit", Deotaland.startAsr("selftest"), ESP_ERR_NOT_SUPPORTED);
  check("recording-core-limit", Deotaland.startRecording("test.wav", blob, sizeof(blob)),
        ESP_ERR_NOT_SUPPORTED);
#endif
  check("selected-agent-upstream-limit", Deotaland.selectAgent("agent-1"), ESP_ERR_NOT_SUPPORTED);
  check("video-upstream-limit", Deotaland.pushVideo(blob, sizeof(blob), 0, true),
        ESP_ERR_NOT_SUPPORTED);

  check("runtime-register", Deotaland.registerIo(runtime_endpoint), ESP_OK);
  check("manifest-notify", Deotaland.notifyManifestChanged(), ESP_OK);
  check("runtime-reading", Deotaland.pushReading("runtime0", &u16, sizeof(u16)), ESP_OK);

  Serial.printf("SELFTEST:CONNECTED_DONE passed=%d failed=%d\n", passed, failed);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
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
  callbacks.on_custom = onCustom;
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
  config.firmware_rev = "0.2.0-test";

  check("begin", Deotaland.begin(config), ESP_OK);
  check("begin-twice", Deotaland.begin(config), ESP_ERR_INVALID_STATE);
  checkBool("state-disconnected", Deotaland.state() == AGENT_STATE_DISCONNECTED);
  check("battery-range", Deotaland.reportBattery(101, false), ESP_ERR_INVALID_ARG);
  check("reading-null", Deotaland.pushReading("bool0", nullptr, 1), ESP_ERR_INVALID_ARG);
  check("reading-size", Deotaland.pushReading("bool0", &failed, sizeof(failed)),
        ESP_ERR_INVALID_STATE);  // link-state validation occurs before type-size validation
  check("event-null", Deotaland.pushEvent(AGENT_EVT_BUTTON, nullptr, 1), ESP_ERR_INVALID_ARG);
  check("voice-null", Deotaland.pushVoiceBytes(nullptr, 1), ESP_ERR_INVALID_ARG);
  check("voice-end-idempotent", Deotaland.endVoice(), ESP_OK);
  check("asr-push-before-start", Deotaland.pushAsr(reinterpret_cast<const uint8_t*>("x"), 1),
        ESP_ERR_INVALID_STATE);
  check("asr-end-idempotent", Deotaland.endAsr(), ESP_OK);
  check("manifest-offline", Deotaland.notifyManifestChanged(), ESP_OK);
  Serial.printf("SELFTEST:BOOT_DONE passed=%d failed=%d\n", passed, failed);
}

void loop() {
  if (Deotaland.isReady() && !link_tests_done) {
    link_tests_done = true;
    delay(500);  // let the central finish enabling notifications
    runConnectedTests();
  }
  delay(10);
}
