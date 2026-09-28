# deotaland-arduino

Arduino Agent Link 通信库，当前版本 **0.6.1**。通过 BLE 上报传感器、接收控制命令、发送提示词，并支持 Wi-Fi 联网、配网和平台激活。内置后端面向 ESP32；STM32 需使用自定义传输适配器。

## 快速开始

1. 在 Arduino IDE 中通过“Sketch → Include Library → Add .ZIP Library”安装，或将本目录放入 Sketchbook 的 `libraries`。
2. 选择实际板型、端口、Flash 和分区。已验证基线为 ESP32-S3 + Arduino-ESP32 3.3.11，S3 使用 `PartitionScheme=min_spiffs`。
3. 从“文件 → 示例 → deotaland-arduino”打开下面的示例，上传后以 **115200** 查看串口。

BLE 需要芯片/Core 支持 NimBLE 及兼容 Agent Link 的 App；Wi-Fi 需要带 Wi-Fi 的 ESP32。详细安装、USB 设置和排错见 [中文入门指南](docs/BEGINNER-GUIDE.zh-CN.md)。

## 选择示例

| 目标 | 示例 |
| --- | --- |
| 验证 BLE 连接 | [BasicBLE](examples/BasicBLE/BasicBLE.ino) |
| 上报读数、接收开关控制 | [CachedSensor](examples/CachedSensor/CachedSensor.ino) / [SensorActuator](examples/SensorActuator/SensorActuator.ino) |
| 发送文字提示词 | [SerialPrompt](examples/SerialPrompt/SerialPrompt.ino) |
| 填写 SSID/密码直连路由器 | [BasicWiFi](examples/BasicWiFi/BasicWiFi.ino) |
| 通过设备热点配网 | [WiFiProvisioning](examples/WiFiProvisioning/WiFiProvisioning.ino) |
| 配置平台并激活、绑定 | [CloudActivation](examples/CloudActivation/CloudActivation.ino) |

读数优先使用 `updateBool / updateInt32 / updateUInt16 / updateFloat / updateText`，库负责类型检查、缓存和后续发送。音频、图片、播放及移植等全部 **16 个示例**见 [示例索引](examples/README.zh-CN.md)。

## 使用要点

- 每轮 `loop()` 调用 `Deotaland.poll()`，回调快速返回；检查启动和发送 API 的返回值。
- BLE 订阅事件特征 `0xFFC4` 后才是 `READY`；AUDIO、IMAGE、TTS 和 OTA 另需启用 CoC 的 Core 及对应通道。
- **Wi-Fi 尚无控制/媒体数据通道，`isReady()` 始终为 false。** 用 `status().wifi_connected`、`wifi_ip` 和 `wifi_portal_active` 查看联网与配网状态。
- `ESP_OK` 表示本地接受，不保证对端收到；缓存读数在离线或拥塞时只保留最新值。
- 示例使用模拟数据或测试音；真实传感器、麦克风、扬声器等驱动需按硬件接入。

## 文档与验证

[入门指南](docs/BEGINNER-GUIDE.zh-CN.md) · [API 与进阶参考](docs/REFERENCE.zh-CN.md) · [常见问题](docs/BEGINNER-GUIDE.zh-CN.md#troubleshooting)

[0.6.1 验证记录](tests/VALIDATION-0.6.1.md) 包含主机回归、Host ASAN、8 个 S3 示例及 H2/STM32 两个编译目标。编译通过不代表真实无线、音视频或 OTA 已完成硬件验证。

测试与打包方法见 [tests/README.md](tests/README.md)。许可证见 [LICENSE](LICENSE)。
