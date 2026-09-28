/*
 * Wi-Fi 入门：填写 SSID/密码，直接连接路由器，不需要手机 BLE 配对。
 * 适用：有 Wi-Fi 的 ESP32；串口监视器设为 115200。
 * 修改下面两项后上传，看到“Wi-Fi 已连接，IP:”即完成本例目标。
 * 连接失败时库会重试，必要时打开 Deotaland-WiFi-XXXX 配网热点。
 * 本例验证连接路由器；要做云端激活/绑定，再看 CloudActivation。
 * 当前 Wi-Fi 后端没有 Agent 控制/音视频数据通道，isReady() 不会为 true。
 */
#include <Deotaland.h>
#include <string.h>

// 修改点：填写自己的网络。开放网络的密码填 ""，不要在日志中输出密码。
const char kSsid[] = "YOUR_WIFI_SSID";
const char kPassword[] = "YOUR_WIFI_PASSWORD";

namespace {
bool started = false;
bool configured = false;
bool printRequested = true;
uint32_t lastLog = 0;
esp_err_t startError = ESP_OK;
void printStatus() {
  if (!configured) {
    Serial.println("请先修改草图顶部的 kSsid/kPassword，再重新上传。");
    return;
  }
  if (!started) {
    Serial.print("begin: "); Serial.println(Deotaland.errorName(startError));
    Serial.println(Deotaland.errorHint(startError));
    return;
  }

  const auto status = Deotaland.status();
  if (status.phase == AGENT_LINK_PHASE_BLOCKED && status.detail != 0) {
    Serial.print(status.title); Serial.print(": "); Serial.println(status.hint);
    Serial.print("detail: "); Serial.println(status.detail);
    return; // 后台启动失败时，不再误报“正在连接”。
  }

  // 库提供独立的网络快照，不需要安装事件监听器，也不要再调用 WiFi.begin()。
  // wifi_connected 只代表获取了路由器 IP，不保证互联网/TLS 或 Agent 数据通道可用。
  if (status.wifi_connected) {
    Serial.print("Wi-Fi 已连接，IP: "); Serial.println(status.wifi_ip);
  } else {
    Serial.println("尚未获取 IP：检查 SSID、密码和信号，并查看下面的连接/配网提示。");
  }
  Serial.print(status.title); Serial.print(": "); Serial.println(status.hint);
  if (status.wifi_portal_active)
    Serial.println("连接上面提示的热点，然后打开 http://192.168.4.1/ 完成配网。");
  if (status.phase == AGENT_LINK_PHASE_NETWORK_READY)
    Serial.println("本例连接路由器的目标已完成。云端激活请继续学习 CloudActivation。");
}
} // namespace

void setup() {
  Serial.begin(115200);
  configured = kSsid[0] != '\0' && strcmp(kSsid, "YOUR_WIFI_SSID") != 0 &&
      strcmp(kPassword, "YOUR_WIFI_PASSWORD") != 0;
  if (!configured) return; // 避免拿占位符反复连接；loop 会重复显示修改提示。

  agent_wifi_config_t wifi = {kSsid, kPassword};
  agent_link_config_t config = {};
  config.device_name = "Deotaland-WiFi";
  config.firmware_rev = "1.0.0";
  config.transport = AGENT_TRANSPORT_WIFI; // 核心：选择 Wi-Fi 模式。
  config.wifi = &wifi;                    // 核心：提供预设网络凭据。
  // 显式 SSID 每次启动优先于 NVS。通过门户换网后也要更新这里，或改用 WiFiProvisioning。
  // begin 会复制 wifi 和字符串，局部变量 wifi 在 setup 结束后可安全销毁。
  startError = Deotaland.begin(config);
  started = startError == ESP_OK; // 成功表示后台任务启动，仍需等待获取 IP。
}

void loop() {
  if (started) Deotaland.poll(); // 持续推进状态，不用 while 等待连接。
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n)
    if (Serial.read() == '?') printRequested = true;
  const uint32_t now = millis();
  if ((printRequested && uint32_t(now - lastLog) >= 500) || uint32_t(now - lastLog) >= 5000) {
    printRequested = false;
    lastLog = now;
    printStatus(); // 低频输出；稍后打开串口也能看到状态。? 可主动查询。
  }
}
