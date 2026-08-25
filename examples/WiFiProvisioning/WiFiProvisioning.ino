#include <Deotaland.h>
#include <esp_wifi.h>

void setup() {
  Serial.begin(115200);

  agent_link_config_t config = {};
  config.device_name = "Deotaland-WiFi";
  config.transport = AGENT_TRANSPORT_WIFI;
  // Leave config.wifi null to use saved credentials or open the captive portal.
  // To skip provisioning, supply a static-lifetime agent_wifi_config_t with ssid/password.

  const esp_err_t result = Deotaland.begin(config);
  Serial.printf("WiFi transport start: %s\n", esp_err_to_name(result));
  Serial.println("With no saved credentials, join the open Deotaland-WiFi-XXXX AP.");
  Serial.println("Cloud control/media remain unavailable until the upstream WiFi data plane is implemented.");
}

void loop() {
  static bool reported = false;
  if (!reported && millis() > 2000) {
    reported = true;
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t ap = {};
    const esp_err_t mode_result = esp_wifi_get_mode(&mode);
    const esp_err_t ap_result = esp_wifi_get_config(WIFI_IF_AP, &ap);
    Serial.printf("WiFi mode=%d (%s), SoftAP=%s channel=%u (%s)\n",
                  static_cast<int>(mode), esp_err_to_name(mode_result),
                  reinterpret_cast<const char*>(ap.ap.ssid), ap.ap.channel,
                  esp_err_to_name(ap_result));
  }
  delay(1000);
}
