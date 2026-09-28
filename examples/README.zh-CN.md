# 示例学习索引

每个目录内的同名 `.ino` 可直接在 Arduino IDE 打开。先按 [中文入门指南](../docs/BEGINNER-GUIDE.zh-CN.md) 配置开发板和串口。示例声明的端点/能力是练习协议用的，不意味着相应物理设备已经接入。

| 示例 | 层级与目标 | 前置条件 | 默认数据/硬件替换点 |
| --- | --- | --- | --- |
| [BasicBLE](BasicBLE/BasicBLE.ino) | 1：检查启动、连接和电量 | BLE/NimBLE；兼容 App，订阅 0xFFC4 | 固定模拟电量 80%；无 GPIO |
| [CachedSensor](CachedSensor/CachedSensor.ino) | 2：最简单的温度上报 | 先跑通 BasicBLE | 模拟温度；替换 readTemperatureC |
| [SensorActuator](SensorActuator/SensorActuator.ino) | 3：传感器 + 软件开关 | App 支持 I/O 清单与控制 | kLedPin=-1；确认引脚及有效电平后启用普通 LED |
| [SerialPrompt](SerialPrompt/SerialPrompt.ino) | 4：发送短提示词、重试与取消 | READY；App/平台提供 Agent 业务 | 串口 p 发送“你好”；? 看状态，忽略换行 |
| [BasicWiFi](BasicWiFi/BasicWiFi.ino) | Wi-Fi 入门：预设凭据直连路由器 | 有 Wi-Fi 的 ESP32；修改 kSsid/kPassword | 通过库状态查看 IP/配网进度；不需要 BLE，不配置云平台 |
| [WiFiProvisioning](WiFiProvisioning/WiFiProvisioning.ino) | 分支：独立配网 | 有 Wi-Fi 的 ESP32 | 没有云端数据通道；无凭据时启用热点 |
| [CloudActivation](CloudActivation/CloudActivation.ino) | 分支：激活、绑定、云状态 | 自己的平台 HTTPS 地址/产品 ID/芯片型号 | 默认占位符；串口 f 清本地平台凭据 |
| [AdvancedIO](AdvancedIO/AdvancedIO.ino) | 进阶：多端点、动态清单、即时发送 | 完成 CachedSensor；理解 pushReading 的手动重试 | 模拟温度/固定启动次数；mode 仅修改软件状态 |
| [ResponsiveLoop](ResponsiveLoop/ResponsiveLoop.ino) | 进阶：时间预算与动作状态机 | 调试器或自己的低频统计输出 | GPIO 默认 -1；观察循环/预算计数器 |
| [VoicePTT](VoicePTT/VoicePTT.ino) | 进阶：按住说话的会话流程 | READY + 订阅 0xFFA1；TTS 另需 CoC | 串口 p/r；实体按键默认关闭；makeTestPcm 是测试音，onAudio 不播放 |
| [RealtimeASR](RealtimeASR/RealtimeASR.ino) | 进阶：App 发起 AUDIO 采集 | CoC Core；App 发出开始/结束采集命令 | makeTestPcm 是测试音；不含麦克风驱动 |
| [BufferedPlayback](BufferedPlayback/BufferedPlayback.ino) | 进阶：PCM 环形缓冲与播放背压 | TTS 下行、CoC、自己的 I2S/DMA 驱动 | 默认 boardWritePcmNonBlocking 返回 0；需实现写入/真实水位/取消 |
| [ImageStream](ImageStream/ImageStream.ino) | 进阶：图片流分步重试 | CoC Core；App 打开 IMAGE 通道 | 固定 2x2 RGB565；真实相机需给出尺寸、格式和长度 |
| [CustomTransport](CustomTransport/CustomTransport.ino) | 移植：UART 桥接 | 足够 RAM 的 32 位板、自有桥接程序 | 官方 App 不识别 UART 包装；检查串口、稳定 device_id |
| [LifecycleSelfTest](LifecycleSelfTest/LifecycleSelfTest.ino) | 诊断：停止后再启动 | BLE 目标；115200 串口 | 自动执行生命周期，不是常规业务模板 |
| [HardwareSelfTest](HardwareSelfTest/HardwareSelfTest.ino) | 诊断：ESP32 集成/堆检查 | ESP32；部分步骤需 App 与订阅 | 回调打印，不驱动实际扬声器/屏幕；不等于物理外设自检 |

CoC 指 BLE 的 L2CAP 面向连接通道。控制/VOICE 的成功不能证明 AUDIO/IMAGE/TTS/OTA 也受支持。当前编译使用的 Core 未启用 CoC，相关运行行为需更换正确构建的 Core 再联调。

BLE 路线：BasicBLE → CachedSensor → SensorActuator / SerialPrompt → 你的真实外设 → 进阶流。

Wi-Fi 路线：BasicWiFi（填写凭据直连）或 WiFiProvisioning（热点配网）→ CloudActivation（配置平台并激活）。Wi-Fi 有 IP、云端在线和 Agent 数据通道 READY 是不同阶段，当前只支持前两者。发生错误按 [排查表](../docs/BEGINNER-GUIDE.zh-CN.md#troubleshooting) 处理，不通过加长 delay 或持续 begin 掩盖问题。
