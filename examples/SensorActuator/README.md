# SensorActuator：读数上报与开关控制

演示设备向 App 上报运行时间和开关状态，并接收 App 的 BOOL 控制命令。默认只改变软件状态，确认 LED 接线后可扩展为真实 GPIO 输出。

## 准备与配置

- 带 BLE 的 ESP32、官方 ESP32 Arduino Core、本库，以及支持 Agent Link I/O 清单和控制的 App。
- 建议先完成 [BasicBLE](../BasicBLE/README.md) 和 [CachedSensor](../CachedSensor/README.md)。
- 默认 `kLedPin=-1`，无需外接 LED。
- 接普通 GPIO LED 时，把 `kLedPin` 改为实际引脚，并根据接线设置 `kLedActiveLow`。地址式 RGB LED 需要自己的驱动，不能直接使用这里的 `digitalWrite()`。

## 使用步骤

1. 打开 [SensorActuator.ino](SensorActuator.ino)，按实际板型和端口编译上传。
2. 打开 **115200** 串口，在 App 中连接 **`Deotaland-IO`** 并订阅事件特征 `0xFFC4`。
3. 在 App 的 I/O 控制界面把 `led0` 设置为 `true` 或 `false`。
4. 查看串口的 `开关目标: ON/OFF`，以及 App 中的 `enabled0` 和 `uptime0`。

本例没有串口控制命令。默认启动后会应用一次关闭状态。

## 端点

| ID | 方向 / 类型 | 用途 |
| --- | --- | --- |
| `uptime0` | IN / I32 | 每秒更新一次运行秒数，达到 I32 上限后保持上限 |
| `enabled0` | IN / BOOL | 回传当前应用的逻辑开关状态 |
| `led0` | OUT / BOOL | App 设置开关目标值 |

IN 表示从设备采集并上报，OUT 表示由 Agent 控制设备输出。BOOL 命令的载荷是一个 `0` 或 `1` 字节，不能把字符串 `"true"` 直接当成 BOOL 载荷。

## 预期结果与扩展

回调只记录目标值，`loop()` 再应用 GPIO 和更新缓存。重复设置同一目标不会反复翻转开关。每轮 `loop()` 都调用 `poll()`，不等待网络发送完成。

`enabled0` 反映软件状态；若需要证明实际灯亮、继电器动作或故障，应接入真实反馈并增加读数。`updateBool()` / `updateInt32()` 的 `ESP_OK` 表示本地缓存成功，不是对端确认。

I/O 描述符保持全局生命周期。接口说明见[API 参考](../../docs/REFERENCE.zh-CN.md)，其他示例见[索引](../README.zh-CN.md)。
