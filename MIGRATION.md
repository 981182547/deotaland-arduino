# Agent_link 新版到 Arduino 0.2.0 的迁移说明

## 代码映射

| ESP-IDF 新版 | Arduino 库 |
|---|---|
| `components/agent_link/include/*.h` | `src/agent_link*.h`，保留 C API |
| `components/agent_link/src/*` | `src/*`，按 Arduino Core 做条件适配 |
| `main/app_main.cpp` | `DeotalandClass` + `examples/*.ino` |
| `boards/*` | 通用行为移植为 `.ino`；硬件专用源码同步到 `extras/idf-reference/boards` |
| 配网页嵌入资源 | `wifi_provision.cpp` 内置页面，避免依赖 ESP-IDF `EMBED_TXTFILES` |

## 相比旧 Arduino 版新增

- `AGENT_VAL_STR`，以及 `display_name`、`audience`、`enum_json`、`default_json`、`event` 清单字段。
- 带 `proto/rev/caps/io` 的版本化清单、运行时注册和 `notifyManifestChanged()`。
- GetReading、SetReadingConfig、读数缓存、按变化上报和速率限制。
- 通用 `pushEvent()`；按钮、传感器、唤醒词和自定义事件均已接线。
- App 发起监听 `on_listen` 及 `startAsr/pushAsr/endAsr`。
- LED、震动、文本专用回调对应的合成执行器端点。
- Wi-Fi STA、NVS 凭据、重连、失败回退和 SoftAP 配网页。
- 新增 Arduino 示例；同步上游 ES8311 ASR/语音、GC2145 和 RoRoLee Basic 硬件参考源码。
- 旧版 `recording_*` 和 Arduino `startRecording/pushRecording/endRecording` 继续可用。

## Arduino 专用修复

- Arduino-ESP32 3.3+ 会回收未声明使用的 BLE 控制器内存。库现在使用官方 `esp32-hal-alloc-ble-mem.h` 标记，避免 ESP32-S3 在 `nimble_port_init()` 中崩溃；旧 Core 通过 `__has_include` 兼容。
- NimBLE GAP/GATT 标准服务由 `nimble_port_init()` 初始化，库不再重复初始化。
- BLE 状态在连接时为 `CONNECTED`，启用 0xFFC4 通知后为 `READY`，避免订阅前发送的数据丢失。
- 控制通知使用 64 KiB 有界队列和独立任务按序重试，避免事件突发与命令响应并发时的 mbuf 拥塞丢包。
- `end()` 会停止广播/连接、关闭 NimBLE 并支持再次 `begin()`。
- NVS 需要恢复时不擅自擦除用户 `Preferences` 数据。
- L2CAP CoC 依 `CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM` 条件编译；标准 Core 关闭时返回明确错误且不引用被裁剪符号。

## 功能边界

- 标准 Arduino Core 的 NimBLE CoC 数量为 0，因此实时 ASR/录音/TTS 下行代码虽已移植，无法在官方预编译 Core 上运行。
- Wi-Fi 本地连接和配网已实现；上游没有提供云端控制/媒体协议实现，因此 Wi-Fi 数据面保持 `ESP_ERR_NOT_SUPPORTED`。
- `report_selected_agent()`、视频上传、图像/Agent 列表标准命令仍是上游未完成项。
- 加密标志可解析，但上游未实现应用层加解密。
- GC2145、ES8311、显示和电量计例程需要对应硬件组件；因此保留在 `extras/idf-reference`，不会给所有 Arduino 草图强加依赖。

## ESP32-S3 验证

- 环境：COM18，ESP32-S3 rev 0.2，Arduino-ESP32 3.3.11，Arduino CLI 1.5.1。
- 所有 8 个 Arduino 示例使用 `--warnings all` 编译并逐个烧录到 COM18。
- BLE 综合自检：启动 `36/0`，连接后累计 `59/0`。
- Windows BLE 中心端：身份 UUID、服务发现、DIS/Battery、三路通知、命令、执行器、监听、清单、读数、事件和语音全部通过，`failures=0`。
- 生命周期：首次 `begin`、`end`、重复 `end`、二次 `begin` 全部通过。
- Basic/Sensor/Advanced：READY、自定义命令、电池、动态清单、执行器、周期读数和 GetReading 均通过。
- VoicePTT：串口驱动同一 PTT 分支，收到 241 个 VoiceChunk；TTS 结束和 `endVoice` 通过。
- RealtimeASR：Start/StopCapture 通过；CoC 关闭时只返回一次 `ESP_ERR_NOT_SUPPORTED`，不会在 loop 中重复重试。
- Wi-Fi：`WIFI_MODE_APSTA`，SoftAP `Deotaland-WiFi-8618`，信道 1，配置结果 `ESP_OK`；Windows 外部扫描可见、信号 99%。
