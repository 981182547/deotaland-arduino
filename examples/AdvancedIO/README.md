# AdvancedIO：多端点、动态清单与手动重试

[源码](AdvancedIO.ino)演示注册多个 I/O 端点、启动后增加端点，以及用 `pushReading()` 分步发送读数。
它适合在完成 CachedSensor 后学习即时发送接口；温度、启动次数都是演示数据，模式控制只改变软件状态。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- 兼容 Agent Link I/O 清单、读数和执行器命令的 App。
- 无需传感器、按键或执行器接线；控制与读数不需要 CoC。
- 先按[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)跑通 BasicBLE。

## 配置与硬件替换点

| 端点 | 方向与类型 | 本例行为 |
| --- | --- | --- |
| `temp0` | IN / F32，单位 C | 每秒生成 20.0～20.9 的循环演示值 |
| `status0` | IN / STR | 上报当前模式文字，初始为 `normal` |
| `mode0` | OUT / STR | 接受 `quiet`、`normal`、`boost` |
| `boots0` | IN / I32 | 启动约 10 秒后注册，固定上报 1 |

替换 `pending_temperature` 的赋值接入真实传感器；把固定 `count = 1` 改为自己的持久化计数。
`boots0` 不是 NVS 启动次数，`actuate()` 也没有电机、风扇等驱动。
在 `actuate()` 接收模式目标后，可让 `loop()` 推进实际硬件动作，回调应快速返回。

描述符和字符串使用全局/static 存储，注册后必须持续有效。
STR 参数是带长度的 UTF-8 字节，不带 JSON 引号，也不保证以 NUL 结尾；本例用长度和 `memcmp()` 比较。
`enum_json` 是清单说明，实际允许的模式由回调检查。

## 上传与操作

1. 打开 [AdvancedIO.ino](AdvancedIO.ino)，按真实板卡选择芯片、Flash、分区和 USB/串口配置。
2. 上传后打开 115200 串口监视器；必要时复位以看到启动输出。
3. 在 App 连接 `Deotaland-AdvancedIO`，订阅事件特征 `0xFFC4`，使控制链路进入 READY。
4. 查看 `temp0` 和 `status0`；等待启动约 10 秒，清单会增加 `boots0`。
5. 在 App 把 `mode0` 设为上述三个字符串之一，观察后续 `status0` 读数变化。

本例没有串口交互命令。App 的执行器操作对应 `IoActuate (0x33)`；清单查询可使用 `GetIoManifest (0x34)`。
若直接构造命令，`mode0` 的参数是例如 `boost` 的原始 UTF-8 字节。

## 预期结果与局限

成功启动时串口输出 `begin: ESP_OK`，动态注册成功时输出：

```text
manifest scheduled after registration
```

读数主要在 App 查看，串口不会逐条打印。动态注册只尝试一次；失败会输出 `runtime registerIo: ...`。
程序每轮只尝试一个端点，发送尝试至少间隔 100 ms；队列满或超时时保留当前值，在后续循环重试。
每组发送结束约 1 秒后生成下一组数据，因此不是精确的 1 Hz 硬件采样器。
`pushReading()` 的成功只表示本地接受；实际传输受连接、MTU 和 App 行为影响。

接口约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)，其他练习见[示例索引](../README.zh-CN.md)。
