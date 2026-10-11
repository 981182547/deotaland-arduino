# CachedSensor：温度采样与最新值缓存

演示如何注册一个温度端点，用 `updateFloat()` 更新读数，并让库在 `poll()` 中发送最新值。断线时仍可采样，重连后同步最新读数。

## 准备

- 带 BLE 的 ESP32 开发板、官方 ESP32 Arduino Core、本库及兼容 Agent Link 的 App。
- 建议先运行 [BasicBLE](../BasicBLE/README.md)，确认 App 能订阅事件通知并进入 `READY`。
- 默认使用模拟温度，无需接线；要测量实际温度，再接入自己的传感器。

## 使用步骤

1. 打开 [CachedSensor.ino](CachedSensor.ino)，选择实际板型、端口及应用分区后上传。
2. 打开 **115200** 串口，在 App 中连接 **`Deotaland-Sensor`** 并订阅事件特征 `0xFFC4`。
3. 在 App 的 I/O 清单中查看温度端点 **`temp0`**。
4. 观察串口每 5 秒打印的温度、连接状态和缓存结果。
5. 断开 App 数秒后重连，观察最新值重新同步。

本例没有串口控制命令，采样会自动进行。

## 数据与配置

| 项目 | 默认行为 |
| --- | --- |
| 端点 ID | `temp0` |
| 类型 / 单位 | `AGENT_VAL_F32` / `C` |
| 采样周期 | 1 秒，由 `loop()` 调度 |
| 模拟读数 | 20.0、20.1……20.9°C，约每 10 秒循环 |

接入真实传感器时，在 `setup()` 初始化驱动，并替换 `readTemperatureC()`。`rate_hz=1` 是端点描述，不会自动安排采样；修改频率也要修改 `loop()` 的计时条件。

## 使用要点

- `updateFloat("temp0", value)` 与端点的 F32 类型对应。更改端点 ID 时，上报调用也要一起改。
- 返回 `ESP_OK` 表示数据已进入本地缓存，发送由持续调用的 `poll()` 推进。
- 缓存只保留最新值，不保存离线期间每次采样的历史。
- NaN、Inf 会被拒绝。真实采样失败时，应保留上次有效值并记录故障，避免把失败当作 0°C。
- `stats().last_error` 是最近一次历史错误，成功后不会自动清零；判断当前连接应看 `state()`。

下一步学习 [SensorActuator](../SensorActuator/README.md)。类型与缓存接口见[API 参考](../../docs/REFERENCE.zh-CN.md)，其他示例见[索引](../README.zh-CN.md)。
