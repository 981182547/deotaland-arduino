#include <Deotaland.h>

namespace {

volatile bool capture_requested = false;
volatile uint32_t capture_deadline = 0;
bool streaming = false;

void onListen(bool start, uint32_t max_ms, void*) {
  capture_requested = start;
  capture_deadline = start && max_ms ? millis() + max_ms : 0;
}

agent_output_cb_t callbacks = {};

// Replace this test tone with 16 kHz mono PCM captured from your microphone/codec.
void makeTestPcm(int16_t* samples, size_t count) {
  static int16_t phase = 0;
  for (size_t i = 0; i < count; ++i) {
    phase += 700;
    samples[i] = (phase & 0x4000) ? 1200 : -1200;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  callbacks.on_listen = onListen;
  const esp_err_t result = Deotaland.begin(
      "Deotaland-ASR", AGENT_CAP_MIC | AGENT_CAP_RECORDING, &callbacks);
  Serial.printf("begin: %s\n", esp_err_to_name(result));
#if !defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) || CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM == 0
  Serial.println("ASR API is available, but this Arduino Core was built without NimBLE L2CAP CoC.");
#endif
}

void loop() {
  if (capture_deadline && static_cast<int32_t>(millis() - capture_deadline) >= 0) {
    capture_requested = false;
    capture_deadline = 0;
  }

  if (capture_requested && !streaming && Deotaland.isReady()) {
    const esp_err_t result = Deotaland.startAsr("arduino-asr");
    Serial.printf("startAsr: %s\n", esp_err_to_name(result));
    streaming = result == ESP_OK;
    if (!streaming) {
      // A failed start (for example, a Core built without L2CAP CoC) is a
      // completed capture attempt. Do not retry and flood the serial log on
      // every loop iteration; wait for the next StartCapture command.
      capture_requested = false;
      capture_deadline = 0;
    }
  }
  if ((!capture_requested || !Deotaland.isReady()) && streaming) {
    Serial.printf("endAsr: %s\n", esp_err_to_name(Deotaland.endAsr(Deotaland.isReady())));
    streaming = false;
  }
  if (streaming) {
    int16_t pcm[320];  // 20 ms at 16 kHz
    makeTestPcm(pcm, 320);
    const esp_err_t result = Deotaland.pushAsr(
        reinterpret_cast<const uint8_t*>(pcm), sizeof(pcm));
    if (result != ESP_OK) {
      Serial.printf("pushAsr: %s\n", esp_err_to_name(result));
      Deotaland.endAsr(false);
      streaming = false;
    }
    delay(20);
  } else {
    delay(10);
  }
}
