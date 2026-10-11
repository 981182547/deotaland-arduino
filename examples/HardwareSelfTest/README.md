# HardwareSelfTest：ESP32 接口与连接诊断

[源码](HardwareSelfTest.ino)通过 `SELFTEST` 日志检查参数校验、I/O 注册、堆完整性和连接后的发送接口。
虽然名称含 Hardware，本例**没有物理外设自检驱动**：扬声器、屏幕、灯、电机和麦克风回调只打印或计数。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库；本例使用 ESP32 堆检查 API。
- USB/串口连接和 115200 串口监视器，建议先跑通 BasicBLE。
- 连接阶段需要兼容 Agent Link 的 App；启动阶段检查不需要 App 或外设接线。
- App 订阅事件 `0xFFC4` 进入 READY；VOICE 测试还需订阅 `0xFFA1`。
- 要查看命令响应，App/调试工具还需订阅控制特征 `0xFFC1`。

安装与分区设置见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)。
本例不需要另装 NimBLE-Arduino；CoC 后端说明见 [CoC 文档](../../docs/BLE-COC.zh-CN.md)。

## 配置与自检范围

草图注册 9 个输入端点，覆盖 BOOL、I32、U16、F32、VEC2、VEC3、RGB、BLOB、STR，另有输出 `relay0`。
连接后增加 `runtime0`，检查动态注册、清单通知和读数发送。
所有值都是固定测试值；`relay0` 只打印收到的参数，不会控制继电器。

| 阶段 | 实际检查 |
| --- | --- |
| 启动 | 堆完整性、空参数、重复注册/启动、离线行为、非法流类型/句柄 |
| READY 后 | 各类型读数、电量、事件、VOICE 的打开/写入/关闭、提示词、动态清单 |
| 能力边界 | 不支持的 VIDEO、FILE、selectAgent；查询 AUDIO/IMAGE 后端能力 |
| 手动命令 | 文本、灯、电机、采集请求、执行器与私有命令的回调派发 |

VOICE 仅写入 320 字节全零 PCM，不采集麦克风。
支持 CoC 时，AUDIO/IMAGE 的实际传输会输出 `SELFTEST:SKIP`，需要单独的对端会话测试。
没有相机拍照、扬声器播放、OTA 刷写、无线吞吐或长时间内存泄漏测试。

可按实际产品修改 `model`、`firmware_rev` 和能力声明；默认型号文字为 `ESP32-S3 SelfTest`。
若接入真实硬件，应另写驱动和健康判据，不能把本例声明的能力当作已检测到外设。

## 上传与操作

1. 打开 [HardwareSelfTest.ino](HardwareSelfTest.ino)，选择正确 ESP32 板型并上传。
2. 打开 115200 串口后复位，等待 `SELFTEST:BOOT_DONE` 再连接 App。
3. 在 App 连接 `Deotaland-SelfTest`，订阅 `0xFFC4` 和 `0xFFA1`。
4. READY 持续约 500 ms 后自动开始连接检查，每约 100 ms 推进一步；暂时拥塞最多重试约 5 秒。
5. 查看最终 `SELFTEST:CONNECTED_DONE passed=... failed=...`，并逐项检查 FAIL 和 SKIP 原因。

本例没有串口交互命令；要重新执行一轮，复位设备。
App 可对 `relay0` 发送 true/false；自动端点 `screen0`、`led0`、`motor0` 分别接收文字、RGB 和毫秒时长。
这些操作只产生 `CALLBACK:text`、`CALLBACK:led`、`CALLBACK:haptic`、`CALLBACK:actuate` 日志。
采集开始/结束命令 `0x3C`/`0x3D` 只打印 `CALLBACK:listen`，不会启动实际录音。
私有命令 `0x7E` 会打印 `CALLBACK:command=0x7E`，并在成功响应的附加数据中返回 `ok!`。

## 预期输出与局限

```text
SELFTEST:START
SELFTEST:PASS:...
SELFTEST:BOOT_DONE passed=... failed=...
SELFTEST:CONNECTED_DONE passed=... failed=...
```

这些省略号表示实际名称/计数；读数是否能放入包会按当时 ATT MTU计算，所以不是固定通过数量。
连接中途丢失会产生 `connection-lost-during-tests` 失败并结束这一轮，不会在新连接继续旧测试。
发送接口的 PASS 只证明本地接受，不能证明 App 收到或外设动作完成；SKIP 也不是已完成媒体验证。
日志和堆诊断可能阻塞，本例适用于联调，不宜直接作为实时音频产品的循环模板。

接口约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)，完整练习见[示例索引](../README.zh-CN.md)。
