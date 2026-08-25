#include <Deotaland.h>

namespace {

constexpr uint8_t kPttPin = 0;  // BOOT on many ESP32-S3 boards; change for your hardware.
bool talking = false;
bool serial_ptt = false;

// Replace this test signal with 16 kHz, signed 16-bit, mono microphone PCM.
void makeTestPcm(int16_t* samples, size_t count) {
  static uint16_t phase = 0;
  for (size_t i = 0; i < count; ++i) {
    phase += 900;
    samples[i] = (phase & 0x8000) ? 900 : -900;
  }
}

void onAudio(const uint8_t*, size_t bytes, void*) {
  // Queue downlink PCM to the board's speaker task here. Do not block this callback.
  Serial.printf("TTS audio: %u bytes\n", static_cast<unsigned>(bytes));
}

void onAudioEnd(void*) { Serial.println("TTS segment ended"); }

agent_output_cb_t callbacks = {};

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kPttPin, INPUT_PULLUP);
  callbacks.on_audio_out = onAudio;
  callbacks.on_audio_end = onAudioEnd;

  const esp_err_t result = Deotaland.begin(
      "Deotaland-Voice", AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_BUTTON,
      &callbacks);
  Serial.printf("begin: %s\n", esp_err_to_name(result));
  Serial.println("PTT: hold BOOT, or send 'p'/'r' over Serial to press/release.");
}

void loop() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'p' || command == 'P') {
      serial_ptt = true;
      Serial.println("Serial PTT pressed");
    }
    if (command == 'r' || command == 'R') {
      serial_ptt = false;
      Serial.println("Serial PTT released");
    }
  }

  const bool pressed = serial_ptt || digitalRead(kPttPin) == LOW;
  if (pressed && !talking && Deotaland.isReady()) {
    talking = true;
    Serial.println("PTT started");
  }

  if (talking && (!pressed || !Deotaland.isReady())) {
    Serial.printf("endVoice: %s\n", esp_err_to_name(Deotaland.endVoice()));
    talking = false;
    delay(20);  // debounce
  }

  if (talking) {
    int16_t pcm[320];  // 20 ms at 16 kHz
    makeTestPcm(pcm, 320);
    const esp_err_t result = Deotaland.pushVoice(pcm, 320);
    if (result != ESP_OK) {
      Serial.printf("pushVoice: %s\n", esp_err_to_name(result));
      Deotaland.endVoice();
      talking = false;
    }
    delay(20);
  } else {
    delay(10);
  }
}
