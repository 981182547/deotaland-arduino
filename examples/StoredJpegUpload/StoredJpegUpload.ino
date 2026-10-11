/* A real JPEG stored in flash -> App, without a camera, filesystem or PSRAM.
 * Target: ESP32 with BLE and the library's bundled NimBLE backend.
 * App must subscribe to control notifications (0xFFC4) and open IMAGE PSM 0x0082.
 * Serial: j = send once, x = cancel, ? = diagnostics (115200 baud).
 * A request belongs to its connection; it is never resumed after reconnect.
 * The const JPEG remains alive for the entire upload. END is local acceptance,
 * not confirmation that the App received or decoded it.
 */
#include <Deotaland.h>
#include "sample_jpeg.h"

namespace {
DeotalandImageUpload upload;
bool started = false;
bool requested = false;
bool interrupted = false;
bool reportRequested = false;
bool wasBusy = false;
esp_err_t beginError = ESP_ERR_INVALID_STATE;
esp_err_t supportError = ESP_ERR_INVALID_STATE;
uint32_t requestedAt = 0;
uint32_t lastReport = 0;

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    requested = false;
    interrupted = true;
  }
}

void report() {
  Serial.println("StoredJpegUpload: j=send JPEG, x=cancel, ?=status");
  Serial.printf("JPEG: %ux%u, %u bytes, generated color/checker test card\n",
                unsigned(kSampleJpegWidth), unsigned(kSampleJpegHeight),
                unsigned(sizeof(kSampleJpeg)));
  if (!started) {
    Serial.print("begin: "); Serial.println(Deotaland.errorName(beginError));
    Serial.println(Deotaland.errorHint(beginError));
    return;
  }
  Serial.print("Control: "); Serial.println(Deotaland.stateName(Deotaland.state()));
  agent_stream_status_t status = {};
  const esp_err_t error = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  Serial.printf("IMAGE support=%s, state=%u, queued=%u, writable=%u\n",
                Deotaland.errorName(error), unsigned(status.state),
                unsigned(status.queued_bytes), unsigned(status.writable_bytes));
  Serial.printf("Uploader: state=%u, accepted=%u/%u, result=%s\n",
                unsigned(upload.state()), unsigned(upload.acceptedBytes()),
                unsigned(sizeof(kSampleJpeg)), Deotaland.errorName(upload.result()));
  Serial.println("READY covers GATT only. IMAGE must also have an App-opened CoC session.");
}

void request(uint32_t now) {
  if (!started || supportError != ESP_OK || !Deotaland.isReady()) {
    Serial.println("Connect the App first; use ? to check IMAGE support.");
    return;
  }
  if (requested || upload.busy()) {
    Serial.println("An image is already pending; x cancels it.");
    return;
  }
  requested = true;
  requestedAt = now;
}

void service(uint32_t now) {
  if (interrupted) {
    interrupted = false;
    upload.cancel();
  }
  upload.poll(now);
  if (wasBusy && !upload.busy()) {
    Serial.print("Upload finished locally: ");
    Serial.println(Deotaland.errorName(upload.result()));
  }
  wasBusy = upload.busy();
  if (!requested) return;
  if (!Deotaland.isReady() || uint32_t(now - requestedAt) >= 30000) {
    requested = false;
    Serial.println("Request expired while waiting for IMAGE; send j to try again.");
    return;
  }
  if (upload.busy()) return;
  agent_stream_status_t status = {};
  const esp_err_t error = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  if (error != ESP_OK) {
    requested = false;
    Serial.print("IMAGE: "); Serial.println(Deotaland.errorName(error));
    return;
  }
  // A successful close can leave bytes draining. Do not overlap another image.
  if (status.state == AGENT_STREAM_UNAVAILABLE || status.state == AGENT_STREAM_OPEN ||
      status.state == AGENT_STREAM_DRAINING) return;
  agent_stream_opts_t options = {};
  options.name = "stored-test-card";
  options.encoding = AGENT_ENC_JPEG;
  options.width = kSampleJpegWidth;
  options.height = kSampleJpegHeight;
  options.total_bytes = sizeof(kSampleJpeg);
  requested = false;
  const esp_err_t result = upload.start(options, kSampleJpeg, sizeof(kSampleJpeg), now);
  wasBusy = upload.busy();
  Serial.print("JPEG start: "); Serial.println(Deotaland.errorName(result));
}
} // namespace

void setup() {
  Serial.begin(115200);
  beginError = Deotaland.begin("Deotaland-StoredJPEG", AGENT_CAP_CAMERA, nullptr, onState);
  started = beginError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    supportError = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  }
  report();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t count = 0; count < 8 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (command == 'j' || command == 'J') request(now);
    else if (command == 'x' || command == 'X') {
      requested = false;
      upload.cancel();
    } else if (command == '?') reportRequested = true;
  }
  service(now);
  if (reportRequested && uint32_t(now - lastReport) >= 500) {
    reportRequested = false;
    lastReport = now;
    report();
  }
  delay(1);
}
