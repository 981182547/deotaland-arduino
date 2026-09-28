/*
 * Wi-Fi 配网：有 Wi-Fi 的 ESP32，默认不填写账号密码。
 * 无已保存凭据时，在手机/电脑连接开放热点 Deotaland-WiFi-XXXX。
 * 打开配网页填写你的网络；若未自动弹出，在浏览器打开 http://192.168.4.1/。
 * 热点无互联网是正常现象，需保持连接完成配网。串口监视器 115200。
 * 此例不连接 Agent 数据通道；isReady() 不会变 true。激活见 CloudActivation。
 */
#include <Deotaland.h>

namespace {
bool started = false, statusChanged = false;
agent_link_status_t latestStatus = {};
uint32_t lastLog = 0;
void onStatus(const agent_link_status_t* status, void*) {
  latestStatus = *status; // 回调指针只在本次调用有效，复制后在 loop 中打印。
  statusChanged = true;
}
}

void setup() {
  Serial.begin(115200);
  agent_link_config_t config = {};
  config.device_name = "Deotaland-WiFi";
  config.transport = AGENT_TRANSPORT_WIFI;
  config.firmware_rev = "1.0.0";
  config.on_status = onStatus;
  // config.wifi == nullptr：用已保存凭据，或启动配网门户。
  // 若要直接填网络，可在这里创建 agent_wifi_config_t wifi={"SSID","密码"};
  // 然后 config.wifi=&wifi; begin 会复制字符串。不要另行调用 WiFi.begin()。
  const esp_err_t result = Deotaland.begin(config);
  started = result == ESP_OK;
  Serial.print("begin: "); Serial.println(Deotaland.errorName(result));
  if (!started) Serial.println(Deotaland.errorHint(result));
}

void loop() {
  if (!started) return;
  Deotaland.poll();
  const uint32_t now = millis();
  if ((statusChanged && uint32_t(now - lastLog) >= 500) || uint32_t(now - lastLog) >= 10000) {
    statusChanged = false;
    lastLog = now;
    Serial.print(latestStatus.title); Serial.print(": "); Serial.println(latestStatus.hint);
    if (latestStatus.detail) { Serial.print("detail: "); Serial.println(latestStatus.detail); }
    // 无需配置平台也能分辨配网、连接中、已获取 IP。
    if (latestStatus.wifi_connected) {
      Serial.print("Wi-Fi IP: "); Serial.println(latestStatus.wifi_ip);
    }
    if (latestStatus.wifi_portal_active)
      Serial.println("连接提示中的设备热点，打开 http://192.168.4.1/；保存成功后热点会关闭。");
    Serial.println("本例仅配网。云端激活见 CloudActivation；Wi-Fi 暂不传提示词/音视频。");
  }
}
