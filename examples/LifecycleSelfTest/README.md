# LifecycleSelfTest：停止后重新启动

[源码](LifecycleSelfTest.ino)自动执行一次 BLE 启动、停止、重复停止和再次启动。
它用于排查后台驱动未收尾造成的重复 `begin()` 错误，不是普通业务或实时循环模板。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- USB/串口连接和 115200 串口监视器；无需其他硬件接线。
- App 不是必需条件。若要观察连接中停止，可另用兼容 Agent Link App 连接。
- 不发送媒体、不需要建立 CoC；不要在同一草图另启动 BLEDevice/NimBLEDevice。

安装与板卡配置见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，BLE 生命周期环境见 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。

## 配置与流程

`config.device_name` 默认为 `Deotaland-Lifecycle`，只声明电量能力，但不主动上报电量。
`config.firmware_rev` 是设备固件版本；可按产品修改，不必与库版本相同。
本例没有 GPIO、传感器、麦克风或扬声器驱动需要替换。

| 相对阶段 | 操作 |
| --- | --- |
| 上电约 1 秒后 | 第一次 `begin()` |
| 启动后约 2 秒 | `end()`，检查 DISCONNECTED，再次 `end()` 检查幂等行为 |
| 停止后至少 500 ms | 尝试第二次 `begin()` |
| 后台仍在收尾 | 遇到 INVALID_STATE，至少间隔 100 ms 重试，等待窗口约 5 秒 |
| 第二次尝试终止 | 打印结果和 DONE，结束自动测试 |

这里实际用 `begin()` 返回值判断后台是否完成；代码没有调用 `canBegin()`。
普通应用建议持续 `poll()`，等 `canBegin()` 为 true 再启动；接口说明见 [API 参考](../../docs/REFERENCE.zh-CN.md)。

## 上传与操作

1. 打开 [LifecycleSelfTest.ino](LifecycleSelfTest.ino)，选择实际板卡和端口后上传。
2. 打开 115200 串口监视器并复位；无需输入串口命令，流程自动执行。
3. 查看 `first-begin`、`first-end`、`idempotent-end`、`second-begin` 的 PASS/FAIL。
4. 若使用 App，可观察 `Deotaland-Lifecycle` 的广播/连接在停止时消失，再次启动后恢复。
5. 再测一轮时复位；本例不会反复循环启动和停止。

## 预期输出与局限

正常情况会出现 `LIFECYCLE:PASS:first-begin`、`LIFECYCLE:PASS:first-end`、
`LIFECYCLE:PASS:idempotent-end`、`LIFECYCLE:PASS:second-begin`，最后为 `LIFECYCLE:DONE`。
连接变化另外打印 `LIFECYCLE:STATE:<数值>`。
第一次启动失败会输出 `LIFECYCLE:DONE:first-start-failed`，不会继续停止/重启检查。

DONE 表示流程结束，需同时查看 PASS/FAIL；第二次启动失败也会打印 DONE。
第二次启动成功后 BLE 保持运行，`loop()` 继续 `poll()`；测试没有在最后再调用 `end()`。
本例只检查一轮 API 与可见状态，不证明所有硬件已释放、长期内存无泄漏或断连恢复业务正确。
其他诊断和业务示例见[示例索引](../README.zh-CN.md)。
