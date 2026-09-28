/* 云端激活练习：先填自己的 HTTPS 平台根地址、正整数 product_id 和芯片型号。
 * api.example.com 是占位符，不是可用的 Deotaland 服务地址。
 * 先完成 Wi-Fi 配网，再根据串口激活码在对应平台绑定；115200 波特率。
 * 云端在线是 CONNECTED，不是 READY；当前 Wi-Fi 后端尚无控制/媒体数据通道。
 * 串口 f 清除本地平台凭据，保留 UUID/Wi-Fi；控制台绑定可能仍需手动解除。
 */
#include <Deotaland.h>

// Fill these from your platform deployment; no built-in server or account.
// Use an ESP32 with WiFi and an application partition >= 1.5 MB. The tested
// ESP32-S3 setting is PartitionScheme=min_spiffs (keeps two OTA app slots).
#ifndef DEOTALAND_CLOUD_PRODUCT_ID
#define DEOTALAND_CLOUD_PRODUCT_ID 0
#endif
const agent_platform_t platform = {
  "https://api.example.com",  // HTTPS origin, no trailing slash
  DEOTALAND_CLOUD_PRODUCT_ID,  // Your positive product_id (or replace this macro)
  "esp32-s3"
};
bool started = false;

void onStatus(const agent_link_status_t* status, void*) {
  // Runs from loop()/poll(), so a display may be updated here.
  Serial.printf("[%d] %s: %s\n", int(status->phase), status->title, status->hint);
  if (status->phase == AGENT_LINK_PHASE_PAIRING)
    Serial.printf("Activation code: %s (%lu seconds)\n", status->code,
                  static_cast<unsigned long>(status->expires_s));
  if (status->detail) Serial.printf("Detail: %d\n", status->detail);
}

void setup() {
  Serial.begin(115200);
  if (platform.product_id <= 0) {
    Serial.println("Set platform.base_url, product_id and chip_type in this sketch first.");
    return;
  }
  agent_link_config_t config = {};
  config.device_name = "Deotaland-Cloud";
  config.model = "MyESP32Device";
  config.firmware_rev = "0.4.0";
  config.transport = AGENT_TRANSPORT_WIFI;
  config.platform = &platform;
  config.on_status = onStatus;
  // config.wifi == nullptr: use saved WiFi or open the captive portal.
  const esp_err_t result = Deotaland.begin(config);
  started = result == ESP_OK;
  Serial.printf("Start: %s\n", esp_err_to_name(result));
  if (result != ESP_OK) Serial.println(Deotaland.errorHint(result));
  Serial.println("Send f to forget platform pairing; UUID and WiFi credentials are kept.");
}

void loop() {
  if (!started) return;
  Deotaland.poll();
  static bool printed_id = false;
  const char* id = printed_id ? nullptr : Deotaland.deviceId();
  if (id) { Serial.printf("Device UUID: %s\n",id); printed_id = true; }
  if (Serial.available() > 0 && Serial.read() == 'f')
    Serial.printf("Forget request: %s\n",esp_err_to_name(Deotaland.forget()));
  // Online is CONNECTED. WiFi streams/events remain unavailable until upstream
  // implements its runtime data plane. Do not wait for isReady() to show the code.
}
