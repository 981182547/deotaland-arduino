// 32-bit Arduino example: UART control transport, without BLE or ESP-IDF.
// A bridge on the other end must exchange [length LE16][Agent Link v1 frame].
// Use a dedicated serial port in a product. Do not print logs on this link.
/* 进阶移植：通过 UART 桥接 Agent Link，适用于 STM32 等有足够 RAM 的 32 位板。
 * 不是直接连接官方 App；需要电脑/其他设备实现对应桥接程序。
 * 请先阅读 SerialAgentTransport.h 的帧格式和引脚/串口选择注释。
 */
#include <Deotaland.h>
#include "SerialAgentTransport.h"

SerialAgentTransport uartTransport(Serial);
uint32_t lastBattery = 0;
uint32_t lastAttempt = 0;
bool started = false;
// Replace with a persistent, unique ID for each physical device.
const uint8_t deviceId[16] = {0x44,0x65,0x6f,0x74,0x61,0x6c,0x61,0x6e,0x64,0,0,0,0,0,0,1};

void setup() {
  Serial.begin(115200);
  agent_link_config_t config = {};
  config.device_name = "My-Arduino-Device";
  config.caps = AGENT_CAP_BATTERY;
  config.transport = AGENT_TRANSPORT_CUSTOM;
  config.custom_transport = &uartTransport.transport;
  config.device_id = deviceId;
  started = Deotaland.begin(config) == ESP_OK;
  // Indicate startup failure using a board LED/debugger, never this binary UART.
}

void loop() {
  if (!started) return;
  Deotaland.poll(4);
  const uint32_t now = millis();
  if (Deotaland.isReady() && uint32_t(now - lastBattery) >= 5000 &&
      uint32_t(now - lastAttempt) >= 100) {
    lastAttempt = now;
    if (Deotaland.reportBattery(80, false) == ESP_OK) lastBattery = now;
  }
  // Sample sensors and advance actuators here without delay().
}
