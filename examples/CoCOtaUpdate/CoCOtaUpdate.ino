/* App-driven OTA over the shared AUDIO/TTS CoC channel (PSM 0x0081).
 * Target: ESP32 with BLE and an OTA partition layout (ota_0 + ota_1 + otadata).
 * Choose a layout with room for two full application images, such as
 * min_spiffs (1.9 MB per app); Huge APP and single-app IDF layouts cannot OTA.
 * App must implement Agent Link OTA commands and send firmware for this model.
 * Keep kModel stable for a PCB; change kFirmware for each release. The OTA image
 * must be built for the same PCB, and the App's START command must specify the
 * matching model, target revision, byte count and SHA-256 of the application .bin.
 * firmware_rev reports the device revision; it does not change ESP-IDF app metadata.
 * Serial 115200: ?=status, x=cancel, v=mark current firmware healthy after checks.
 * Cancellation/failure requires reconnect before another OTA/audio transfer.
 * No update is started by this sketch; only explicit App OTA commands start it.
 */
#include <Deotaland.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#endif

namespace {
const char kModel[] = "Deotaland-CoC-OTA";
const char kFirmware[] = "1.0.0";
bool started = false;
bool reportRequested = false;
bool progressPending = false;
bool cancelRequested = false;
bool confirmRequested = false;
esp_err_t beginError = ESP_ERR_INVALID_STATE;
esp_err_t prepareError = ESP_ERR_INVALID_STATE;
agent_ota_status_t progress = {};
uint32_t lastReportAt = 0;
uint32_t lastProgressAt = 0;

bool checkPartitions() {
#if defined(ARDUINO_ARCH_ESP32)
  const esp_partition_t* slot0 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* slot1 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* metadata = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (slot0 && slot1 && metadata) {
    Serial.printf("OTA slot capacities: %lu and %lu bytes\n",
                  static_cast<unsigned long>(slot0->size),
                  static_cast<unsigned long>(slot1->size));
    return true;
  }
#endif
  Serial.println("This example needs ota_0, ota_1 and otadata; select an OTA partition layout.");
  return false;
}

void onProgress(const agent_ota_status_t* status, void*) {
  if (status) progress = *status;
  progressPending = true; // No serial output/flash calls inside a callback.
}

void printProgress(const agent_ota_status_t& status) {
  Serial.printf("OTA state=%u received=%lu/%lu error=%u\n", unsigned(status.state),
                static_cast<unsigned long>(status.received_bytes),
                static_cast<unsigned long>(status.total_bytes), unsigned(status.error_code));
}

void report() {
  Serial.println("CoCOtaUpdate: ?=status, x=cancel, v=confirm current firmware health");
  Serial.printf("Model=%s revision=%s running=%s\n", kModel, kFirmware,
                Deotaland.runningVersion());
  Serial.printf("begin=%s prepare=%s supported=%u control=%s\n",
                Deotaland.errorName(beginError), Deotaland.errorName(prepareError),
                unsigned(Deotaland.otaSupported()), Deotaland.stateName(Deotaland.state()));
  const agent_ota_status_t status = Deotaland.otaStatus();
  printProgress(status);
  if (Deotaland.otaNeedsReconnect()) Serial.println("Reconnect before another OTA or AUDIO/TTS transfer.");
  if (prepareError != ESP_OK) {
    Serial.println("Check OTA partitions, free internal heap and the CoC backend.");
    Serial.println(Deotaland.errorHint(prepareError));
  }
  Serial.println("App must open PSM 0x0081 and use a matching model/firmware image.");
}
} // namespace

void setup() {
  Serial.begin(115200);
  agent_link_config_t config = {};
  config.device_name = "Deotaland-CoC-OTA";
  config.model = kModel;
  config.firmware_rev = kFirmware;
  config.enable_ota = true; // Deliberately opt in; normal examples leave OTA disabled.
  config.transport = AGENT_TRANSPORT_BLE;
  Deotaland.setOtaCallback(onProgress);
  beginError = Deotaland.begin(config);
  started = beginError == ESP_OK;
  if (started) prepareError = checkPartitions() ? Deotaland.prepareOta() : ESP_ERR_NOT_FOUND;
  // Preallocation moves setup work out of the App's START command. SDK flash
  // operations may still pause tasks; this example is not a realtime guarantee.
  report();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t count = 0; count < 8 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (command == '?') reportRequested = true;
    else if (command == 'x') cancelRequested = true;
    else if (command == 'v') confirmRequested = true;
  }
  if (cancelRequested) {
    cancelRequested = false;
    Deotaland.abortOta();
    Serial.println("OTA cancellation requested; reconnect after a started update aborts.");
  }
  if (confirmRequested) {
    confirmRequested = false;
    // In a product, call only after its startup/peripheral health checks pass.
    // Here the user confirms manually; no board-specific checks are implied.
    if (Deotaland.otaBusy()) Serial.println("Wait until OTA is idle before health confirmation.");
    else {
      const esp_err_t error = Deotaland.markOtaValid();
      Serial.print("Firmware health confirmation: "); Serial.println(Deotaland.errorName(error));
    }
  }
  if (progressPending && (uint32_t(now - lastProgressAt) >= 500 ||
      progress.state == AGENT_OTA_SUCCESS || progress.state == AGENT_OTA_FAILED)) {
    progressPending = false;
    lastProgressAt = now;
    printProgress(progress);
  }
  if (reportRequested && uint32_t(now - lastReportAt) >= 500) {
    reportRequested = false;
    lastReportAt = now;
    report();
  }
  delay(1);
}
