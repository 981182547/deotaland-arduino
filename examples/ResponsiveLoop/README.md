# ResponsiveLoop：有预算的循环与定时动作

[源码](ResponsiveLoop.ino)演示短回调、定时结束动作、限频发送，以及循环和 poll 耗时统计。
默认 `kDemoLedPin = -1`，不操作 GPIO，也没有真实振动电机驱动。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- 兼容 Agent Link 的 App；连接并订阅事件 `0xFFC4` 后进入 READY。
- 调试器，或自行加入的低频统计展示方式；源码不初始化 Serial，也不打印串口日志。
- 无需外设即可观察软件状态；普通 GPIO LED 是可选演示输出，控制与电量不需要 CoC。

板卡和安装见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，BLE 后端见 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。

## 配置与硬件替换点

设备名称为 `Responsive-Device`，声明 BATTERY 和 HAPTIC 能力。
HAPTIC 回调只记录开始时间和时长，`loop()` 根据时限更新 `hapticActive`。
库会为该回调提供 `motor0` 输出端点，类型 U16、单位 ms。

如需可见输出，把 `kDemoLedPin` 改为实际板卡的普通 GPIO LED 引脚。
示例输出为 HIGH 有效；若实际电路低电平点亮，应对应修改初始化和写入逻辑。
不要直接假定 `LED_BUILTIN` 是普通 GPIO LED；地址灯需要自己的驱动，其耗时也应计入循环。
若接真实振动驱动，应把 GPIO LED 的示范动作换成自己的快速驱动步骤，并按产品要求处理断连取消。

## 上传与操作

1. 打开 [ResponsiveLoop.ino](ResponsiveLoop.ino)，按实际板卡设置上传。
2. 在调试器观察 `started` 和 `startError`，确认 begin 成功；串口空白是本例默认行为。
3. 在 App 连接 `Responsive-Device`，订阅 `0xFFC4`。
4. 对 `motor0` 设置例如 500 ms，观察 `hapticActive` 在时限后变为 false。
5. 启用普通 LED 后，它会在该时段输出 HIGH；默认 GPIO 为 -1 时不会有物理动作。
6. 观察时间统计与发送错误，比较不同硬件驱动或负载下的变化。

本例没有串口命令。App 操作对应 `IoActuate (0x33)`：`motor0` 参数为两字节小端 U16。
READY 时约每 5 秒尝试报告模拟电量 80%、未充电；相同电量可能被库去重。

## 预期统计与局限

| 变量 | 含义 |
| --- | --- |
| `worstLoopUs` | 观察到的最长 loop 执行时间 |
| `worstLoopGapUs` | 两次 loop 开始之间的最长间隔，含调度间隙 |
| `worstPollUs` | `stats().poll_max_us` 的快照 |
| `overBudget` | poll 预算耗尽次数 |
| `lastBatteryError` | 最近一次电量发送结果 |

`poll(4, 1000)` 在工作片之间检查 1 ms 预算，不能抢占已经进入的回调或平台驱动。
这些计数是观测结果，不是硬实时上界；RTOS 调度和同步日志也可能影响循环间隔。
拥塞至少间隔 100 ms 重试，成功或永久错误结束本轮等待下个周期，不在 while 中等发送成功。
动作默认按时长结束，不包含断连立即取消或真实外设故障检测；发送成功也只是本地接受。

预算与回调约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)，其他练习见[示例索引](../README.zh-CN.md)。
