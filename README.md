# deotaland-arduino

`deotaland-arduino` 是 Deotaland Agent Link 的 Arduino ESP32 移植版。0.2.0 已同步当前 `Agent_link-main` 的公开 API、I/O 清单、监听/ASR、Wi-Fi 配网和参考板例程，同时保留旧 Arduino 版录音接口。

## 支持环境

- Arduino-ESP32 3.x（ESP-IDF 5.x），`architectures=esp32`。
- ESP32、ESP32-S3、ESP32-C3、ESP32-C6、ESP32-H2 等带片上 BLE 且启用 NimBLE 的目标。
- ESP32-S2、ESP32-P4 等无片上 BLE 的目标可编译，BLE `begin()` 返回 `ESP_ERR_NOT_SUPPORTED`。
- 库直接使用 Arduino-ESP32 内置 NimBLE，不能和草图中的另一套 BLE/NimBLE 初始化代码同时拥有控制器。

## 已移植功能

| 功能 | Arduino 状态 |
|---|---|
| BLE 广播、固定身份 UUID、GATT 控制/事件服务 | 可用，COM18 ESP32-S3 实测 |
| Device Information、Battery Service | 可用，读写/通知实测 |
| 命令响应、`on_command`、`on_custom`、`on_listen` | 可用，端到端实测 |
| 自描述 I/O、扩展字段、字符串、运行时清单更新 | 可用，端到端实测 |
| GetReading / SetReadingConfig、事件、读数去重/限速 | 可用 |
| LED、震动、文本输出 | 通过合成执行器 `led0`、`motor0`、`screen0` 可用 |
| PCM16 语音上行 | 通过 GATT Notify 可用，端到端实测 |
| Wi-Fi STA、凭据保存、失败回退、SoftAP 配网页 | 可用，ESP32-S3 实测 SoftAP |
| ASR/录音、TTS 下行（L2CAP CoC） | 实现已移植；标准 Arduino Core 默认关闭 CoC，见下方说明 |
| Wi-Fi 云端控制/媒体、BLE+Wi-Fi 混合数据面 | 上游仍未实现，返回 `ESP_ERR_NOT_SUPPORTED` |
| 选择 Agent、视频上传 | 上游仍未实现，返回 `ESP_ERR_NOT_SUPPORTED` |

Arduino-ESP32 官方发行版默认设置 `CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=0`。因此 `startAsr()`、录音上传和 TTS 下行在标准 Core 中会安全返回 `ESP_ERR_NOT_SUPPORTED`；必须使用重新构建并启用 NimBLE L2CAP CoC 的 Arduino Core，仅在草图中定义宏无效。

## 安装

将整个目录复制到 Arduino `libraries` 目录，或在 Arduino IDE 中以 ZIP 库安装。无需额外通用 Arduino 库。

```cpp
#include <Deotaland.h>

void onState(agent_state_t state, void*) {
  Serial.printf("state=%d\n", state);
}

void setup() {
  Serial.begin(115200);
  const esp_err_t result = Deotaland.begin(
      "MyDevice", AGENT_CAP_BATTERY, nullptr, onState);
  if (result != ESP_OK) Serial.println(esp_err_to_name(result));
}

void loop() {
  if (Deotaland.isReady()) Deotaland.reportBattery(80, false);
  delay(5000);
}
```

BLE 物理连接时状态先进入 `AGENT_STATE_CONNECTED`；对端启用事件通知后才进入 `AGENT_STATE_READY`。只有 `isReady()` 为真时才上报读数、事件或语音。

## Arduino API

- 生命周期：`begin()`、`end()`、`state()`、`isReady()`
- I/O：`registerIo()`、`pushReading()`、`notifyManifestChanged()`
- 事件/状态：`pushEvent()`、`reportBattery()`、`selectAgent()`
- 语音：`pushVoice()`、`pushVoiceBytes()`、`endVoice()`
- 实时 ASR：`startAsr()`、`pushAsr()`、`endAsr()`
- 旧版兼容录音：`startRecording()`、`pushRecording()`、`endRecording()`
- 视频接口：`pushVideo()`、`endVideo()`

高级用法可直接包含 `agent_link.h` 使用稳定 C API。回调可能运行在 NimBLE Host 任务中，必须快速返回；显示、播放、传感器总线等耗时操作应转交独立任务。

## 示例

- `BasicBLE`：最小 BLE、状态和电量。
- `SensorActuator`：传感器读数和执行器。
- `AdvancedIO`：新清单字段、字符串、运行时端点和清单修订。
- `VoicePTT`：按键 PTT、PCM16 语音上行和 TTS 回调骨架；也可从串口发送 `p`/`r` 模拟按下/松开。
- `RealtimeASR`：App 发起监听及 ASR 流；标准 Core 会明确提示 CoC 限制。
- `WiFiProvisioning`：Wi-Fi STA/SoftAP 配网页和板端状态输出。
- `LifecycleSelfTest`：`begin/end/begin` 生命周期回归。
- `HardwareSelfTest`：所有通用类型、事件、回调、清单和错误路径综合测试。

`extras/idf-reference/boards` 同步保留上游硬件例程，包括 ES8311 语音/ASR、GC2145 相机、RoRoLee、TemMonitor 等。它们依赖特定引脚和 ESP-IDF 组件，不会被 Arduino 构建器自动编译。

## 真机验证

验证板为 COM18 上的 ESP32-S3（Arduino-ESP32 3.3.11）。8 个示例均以 `--warnings all` 编译并逐个烧录，结果如下：

| 示例 | COM18 真机结果 |
| --- | --- |
| `BasicBLE` | 连接、READY、自定义命令和 100% 电量读回通过 |
| `SensorActuator` | 两端点清单、`uptime0` 周期读数/GetReading、`led0` 开关通过 |
| `AdvancedIO` | `rev:2` 动态清单、4 个端点、三类读数和 `mode0` 执行器通过 |
| `VoicePTT` | TTS 结束回调、PTT 启停和 241 个 BLE VoiceChunk 通过 |
| `RealtimeASR` | Start/StopCapture 通过；标准 Core 下单次返回 `ESP_ERR_NOT_SUPPORTED`，无重复重试 |
| `WiFiProvisioning` | APSTA、开放 AP `Deotaland-WiFi-8618`/信道 1 通过；Windows 扫描信号 99% |
| `LifecycleSelfTest` | 首次 begin、end、重复 end、二次 begin 及重连命令全部通过 |
| `HardwareSelfTest` | 启动 `36/0`、连接累计 `59/0`；中心端 `failures=0` |

综合中心测试还完成了服务发现、订阅、标准服务、命令/响应、执行器、清单、10 个读数、31 个事件和语音通知。`RealtimeASR` 的结果只证明标准 Core 限制分支正确；完整 ASR/录音/TTS 下行仍需启用 NimBLE L2CAP CoC 的自定义 Core。

默认 1.31 MB App 分区可以容纳当前示例，但综合自检约占 85%；集成较大的屏幕、相机或音频库时建议选择更大的 App 分区方案。

更多迁移细节见 [MIGRATION.md](MIGRATION.md)。许可证见 [LICENSE](LICENSE)。
