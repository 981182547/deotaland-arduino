/* Hardware-free BLE CoC diagnostics on an ESP32 with BLE.
 * Uses Deotaland's one Bluetooth stack; do not also call BLEDevice/NimBLEDevice.
 * Serial at 115200: ?=report, i=preallocate IMAGE, a=preallocate AUDIO,
 * v=preallocate GATT VOICE. Reservations are retained until Deotaland.end().
 * This sketch never sends image/audio data or writes firmware.
 * Compile support (ESP_OK) and an App-opened channel (not UNAVAILABLE) are
 * separate checks. TTS shares AUDIO PSM 0x0081; IMAGE uses PSM 0x0082.
 * Try StoredJpegUpload next to test an actual end-to-end picture transfer.
 */
#include <Deotaland.h>
#include <agent_link_transport.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace {
esp_err_t beginError = ESP_ERR_INVALID_STATE;
bool started = false;
bool reportRequested = false;
int prepareRequested = -1;
uint32_t lastReportAt = 0;
uint32_t lastPrepareAt = 0;

const char* streamState(agent_stream_state_t state) {
  switch (state) {
    case AGENT_STREAM_UNAVAILABLE: return "UNAVAILABLE";
    case AGENT_STREAM_IDLE: return "IDLE";
    case AGENT_STREAM_OPEN: return "OPEN";
    case AGENT_STREAM_DRAINING: return "DRAINING";
    case AGENT_STREAM_COMPLETED: return "COMPLETED";
    case AGENT_STREAM_FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

void printStream(const char* label, agent_stream_t kind) {
  agent_stream_status_t status = {};
  const esp_err_t error = Deotaland.streamStatus(kind, &status);
  Serial.printf("%s: support=%s state=%s queued=%u writable=%u\n", label,
                Deotaland.errorName(error), streamState(status.state),
                unsigned(status.queued_bytes), unsigned(status.writable_bytes));
  Serial.printf("  accepted=%lu submitted=%lu rejected=%lu last=%s\n",
                static_cast<unsigned long>(status.accepted_bytes),
                static_cast<unsigned long>(status.sent_bytes),
                static_cast<unsigned long>(status.rejected_writes),
                Deotaland.errorName(status.last_error));
}

void report() {
  Serial.println("CoCDiagnostics: ?=report, i=prepare IMAGE, a=prepare AUDIO, v=prepare VOICE");
  Serial.print("begin: "); Serial.println(Deotaland.errorName(beginError));
  if (!started) {
    Serial.println(Deotaland.errorHint(beginError));
    return;
  }
  Serial.printf("Control=%s ATT_MTU=%u maxPrompt=%u bytes\n",
                Deotaland.stateName(Deotaland.state()),
                unsigned(agent_transport_ble_att_mtu()), unsigned(Deotaland.maxPromptBytes()));
  printStream("VOICE (GATT 0xFFA1)", AGENT_STREAM_VOICE);
  printStream("AUDIO (CoC 0x0081)", AGENT_STREAM_AUDIO);
  printStream("IMAGE (CoC 0x0082)", AGENT_STREAM_IMAGE);
  // AUDIO status probes the shared CoC build capability, not a separate TTS session.
  Serial.println("TTS shares AUDIO's CoC channel. OTA is disabled in this sketch.");
  const agent_link_stats_t stats = Deotaland.stats();
  Serial.printf("RX dropped=%lu highWater=%lu TX retries=%lu pollMaxUs=%lu budgetHits=%lu\n",
                static_cast<unsigned long>(stats.rx_dropped),
                static_cast<unsigned long>(stats.rx_high_water),
                static_cast<unsigned long>(stats.tx_retries),
                static_cast<unsigned long>(stats.poll_max_us),
                static_cast<unsigned long>(stats.poll_budget_exhaustions));
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("Internal heap free=%u largest=%u, PSRAM free=%u\n",
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#endif
  Serial.println("ESP_OK means compiled support. READY needs 0xFFC4 subscription; App opens CoC separately.");
  Serial.println("queued/submitted are local counters, not peer acknowledgements.");
}
} // namespace

void setup() {
  Serial.begin(115200);
  // CAMERA advertises the image protocol for diagnostics; no physical camera is attached.
  beginError = Deotaland.begin("Deotaland-CoC-Diagnostics", AGENT_CAP_CAMERA);
  started = beginError == ESP_OK;
  report();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t count = 0; count < 8 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (command == '?') reportRequested = true;
    else if (command == 'i') prepareRequested = AGENT_STREAM_IMAGE;
    else if (command == 'a') prepareRequested = AGENT_STREAM_AUDIO;
    else if (command == 'v') prepareRequested = AGENT_STREAM_VOICE;
  }
  if (prepareRequested >= 0 && uint32_t(now - lastPrepareAt) >= 500) {
    // Coalesce serial floods and bound log/preallocation work separately from poll.
    lastPrepareAt = now;
    const esp_err_t error = started
      ? Deotaland.prepareStreams(1u << unsigned(prepareRequested)) : ESP_ERR_INVALID_STATE;
    Serial.printf("prepare kind=%u: %s\n", unsigned(prepareRequested), Deotaland.errorName(error));
    prepareRequested = -1;
    reportRequested = true;
  }
  if (reportRequested && uint32_t(now - lastReportAt) >= 500) {
    reportRequested = false;
    lastReportAt = now;
    report();
  }
  delay(1);
}
