# CoCDiagnostics：检查 BLE 流支持、通道和资源

[CoCDiagnostics.ino](CoCDiagnostics.ino) 用串口查看 GATT VOICE、CoC AUDIO 和 IMAGE 的支持情况、连接状态、队列及堆内存，并演示提前预分配流资源。它不发送媒体数据，也不启动 OTA。

## 硬件和 App 前提

- 使用有 BLE 的 ESP32 系列开发板；无需相机、麦克风、扬声器、显示屏或 PSRAM。ESP32-S2 没有 BLE，不适用。
- 安装本库和兼容的官方 Arduino-ESP32 3.x Core；本库自带启用 CoC 的 NimBLE。不要再初始化 `BLEDevice` 或 `NimBLEDevice`。
- 可先不连接 App 检查编入的流支持。检查实际通道时，需要支持 Agent Link 的 App 订阅 `0xFFC4`，并按需打开对应媒体通道。

## 配置与上传

此示例没有需要修改的 GPIO、网络或外设配置；`AGENT_CAP_CAMERA` 仅用于声明图片协议能力，不表示接入了实体相机。

1. 在 Arduino IDE 打开本目录 `.ino`，选择实际开发板和能容纳固件的应用分区。
2. USB 上传，打开 **115200 波特率**串口监视器，查看首次诊断报告。
3. 先输入 `i`，对比 IMAGE 预分配前后的内部堆内存；也可输入 `a` 或 `v` 检查其他流。
4. 用 App 连接 `Deotaland-CoC-Diagnostics`，订阅控制通知，再打开 IMAGE 或 AUDIO CoC 通道。
5. 输入 `?` 对比连接前后的状态。此示例不会自动周期报告，预分配后会再报告一次。

## 串口命令

命令只识别下列小写字母；无需发送完整文本。

| 命令 | 行为 |
| --- | --- |
| `?` | 输出控制状态、ATT MTU、最大提示长度、各流状态、统计和可用内存 |
| `i` | 调用 `prepareStreams()` 预分配 IMAGE 资源 |
| `a` | 预分配 AUDIO 资源 |
| `v` | 预分配 GATT VOICE 资源 |

连续输入会合并请求，预分配和报告均限频至最多每 500ms 一次。重复预分配同一流可复用已有资源；资源保留到 `Deotaland.end()` 的后台清理阶段，本示例不调用 `end()`。

## 怎样读报告

| 字段或状态 | 含义 |
| --- | --- |
| `support=ESP_OK` | 当前构建支持该流的查询；不代表 App 已连接 |
| `UNAVAILABLE` | 当前流不能使用，可能只是对端尚未连接或订阅 |
| `Control=READY` | 控制通知 `0xFFC4` 已就绪；不表示 CoC 已打开 |
| `IMAGE (CoC 0x0082)` | App 需独立打开 IMAGE 通道；打开后空闲流通常为 `IDLE` |
| `AUDIO (CoC 0x0081)` | AUDIO 与 TTS 共用的 CoC 通道，不是独立的 TTS 会话检查 |
| `VOICE (GATT 0xFFA1)` | GATT 语音流，需要对应通知订阅，不依赖 CoC |
| `queued` / `writable` | 本地待发送字节及可写空间 |
| `accepted` / `submitted` / `rejected` / `last` | 本地接受、提交、拒绝写入计数和最近错误 |
| `RX dropped` / `TX retries` / `pollMaxUs` | 接收丢弃、发送重试、最长轮询耗时等运行统计 |
| `Internal heap free/largest` / `PSRAM free` | 内部堆总可用量、最大连续块和可用 PSRAM；没有 PSRAM 时可为 0 |

`prepare ...: ESP_OK` 只代表资源准备成功，不会替 App 打开 CoC，也不发送测试图片。`ESP_ERR_NO_MEM` 时应检查内部 RAM 与连续块，而不能只看 PSRAM 总量。

## 预期现象和限制

启动后即使各流显示 `UNAVAILABLE`，仍可能同时显示 `support=ESP_OK`。App 完成相应订阅或通道连接后，输入 `?` 可看到状态变化。

本地计数不是对端 ACK，本例也不能证明媒体收发、解码、吞吐或 OTA 已通过。实际图片验证可继续运行 [StoredJpegUpload](../StoredJpegUpload/StoredJpegUpload.ino)。

继续阅读：[BLE CoC 说明](../../docs/BLE-COC.zh-CN.md)、[API 参考](../../docs/REFERENCE.zh-CN.md)、[全部示例](../README.zh-CN.md)。
