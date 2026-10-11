# 示例学习索引

现有 22 个示例，每个目录都有 `README.md`，说明作用、准备条件、配置、操作步骤和预期结果。下表链接先打开示例说明，再从说明中的源码链接打开同名 `.ino`；也可直接在 Arduino IDE 的本库示例菜单选择草图。先按 [中文入门指南](../docs/BEGINNER-GUIDE.zh-CN.md) 配置开发板和串口。本库自带启用 CoC 的 NimBLE-Arduino 主机栈，使用兼容的官方 ESP32 Arduino Core 即可，不需要另装 NimBLE-Arduino 或重编 Core。AIOT-Basic 用户可直接从 AiotBasicAgent 整板示例开始；其他示例声明的端点/能力是否接入物理硬件，见下表。

| 示例 | 层级与目标 | 前置条件 | 默认数据/硬件替换点 |
| --- | --- | --- | --- |
| [AiotBasicAgent](AiotBasicAgent/README.md) | 整板应用：语音、TTS、相机、LCD、SD 录音/回放/上传、I/O、OTA | ESP32-S3 AIOT-Basic 独立功放/PDM 麦克风版；PSRAM；原生 USB CDC；兼容 App | 真实硬件引脚按用户提供教程预置；SD 无卡时降级；Wi-Fi 配网/激活可配置切换，媒体使用 BLE |
| [BasicBLE](BasicBLE/README.md) | 1：检查启动、连接和电量 | BLE/NimBLE；兼容 App，订阅 0xFFC4 | 固定模拟电量 80%；无 GPIO |
| [CachedSensor](CachedSensor/README.md) | 2：最简单的温度上报 | 先跑通 BasicBLE | 模拟温度；替换 readTemperatureC |
| [SensorActuator](SensorActuator/README.md) | 3：传感器 + 软件开关 | App 支持 I/O 清单与控制 | kLedPin=-1；确认引脚及有效电平后启用普通 LED |
| [SerialPrompt](SerialPrompt/README.md) | 4：发送短提示词、重试与取消 | READY；App/平台提供 Agent 业务 | 串口 p 发送“你好”；? 看状态，忽略换行 |
| [BasicWiFi](BasicWiFi/README.md) | Wi-Fi 入门：预设凭据直连路由器 | 有 Wi-Fi 的 ESP32；修改 kSsid/kPassword | 通过库状态查看 IP/配网进度；不需要 BLE，不配置云平台 |
| [WiFiProvisioning](WiFiProvisioning/README.md) | 分支：独立配网 | 有 Wi-Fi 的 ESP32 | 没有云端数据通道；无凭据时启用热点 |
| [CloudActivation](CloudActivation/README.md) | 分支：激活、绑定、云状态 | 自己的平台 HTTPS 地址/产品 ID/芯片型号 | 默认占位符；串口 f 清本地平台凭据 |
| [AdvancedIO](AdvancedIO/README.md) | 进阶：多端点、动态清单、即时发送 | 完成 CachedSensor；理解 pushReading 的手动重试 | 模拟温度/固定启动次数；mode 仅修改软件状态 |
| [ResponsiveLoop](ResponsiveLoop/README.md) | 进阶：时间预算与动作状态机 | 调试器或自己的低频统计输出 | GPIO 默认 -1；观察循环/预算计数器 |
| [VoicePTT](VoicePTT/README.md) | 进阶：按住说话的会话流程 | READY + 订阅 0xFFA1；TTS 另需 CoC | 串口 p/r；实体按键默认关闭；makeTestPcm 是测试音，onAudio 不播放 |
| [RealtimeASR](RealtimeASR/README.md) | 进阶：App 发起 AUDIO 采集 | BLE CoC；App 发出开始/结束采集命令 | makeTestPcm 是测试音；不含麦克风驱动 |
| [BufferedPlayback](BufferedPlayback/README.md) | 进阶：PCM 环形缓冲与播放背压 | TTS 下行、CoC、自己的 I2S/DMA 驱动 | 默认 boardWritePcmNonBlocking 返回 0；需实现写入/真实水位/取消 |
| [ImageStream](ImageStream/README.md) | 进阶：图片流分步重试 | BLE CoC；App 打开 IMAGE 通道 | 固定 2x2 RGB565；真实相机需给出尺寸、格式和长度 |
| [StoredJpegUpload](StoredJpegUpload/README.md) | 图片入门：Flash 中的真实 JPEG 分块上传 | ESP32 BLE；App 打开 IMAGE 通道；无 PSRAM 要求 | 原创 32x24 JPEG 测试卡，无相机；串口 j 上传、x 取消、? 看状态；源码缓冲区全程有效 |
| [ImageSequence](ImageSequence/README.md) | 图片进阶：有限序列、进度、取消、等待排空 | ESP32 BLE；App 打开 IMAGE 通道；约 6 KiB 图像 RAM | 3 张 64x48 合成 RGB565 静态图，非视频/真实采集；串口 b/x，App camera0=true/false |
| [CameraSnapshot](CameraSnapshot/README.md) | 真实相机：采集、JPEG 编码、分块上传 | ESP32 相机驱动/PSRAM；BLE CoC；App 打开 IMAGE 通道 | 默认原版 ESP32-S3 GC2145 接线；App 的 camera0、串口 c 或实体按钮触发；见[指南](../docs/IMAGE-UPLOAD.zh-CN.md) |
| [CoCDiagnostics](CoCDiagnostics/README.md) | 诊断：编译能力、通道状态、队列及内存 | ESP32 BLE；115200 串口；连接阶段另需兼容 App | 无外设、不发送媒体/刷写固件；? 查状态，i/a/v 预分配 IMAGE/AUDIO/VOICE；适合首次联调 |
| [CoCOtaUpdate](CoCOtaUpdate/README.md) | OTA：App 固件上传、进度、取消、健康确认 | ESP32 BLE；OTA 双应用分区；App 支持 OTA 协议及 PSM 0x0081 | 明确启用 OTA；配置 PCB model/revision；? 查状态、x 取消、v 手动确认；不自动发起升级 |
| [CustomTransport](CustomTransport/README.md) | 移植：UART 桥接 | 足够 RAM 的 32 位板、自有桥接程序 | 官方 App 不识别 UART 包装；检查串口、稳定 device_id |
| [LifecycleSelfTest](LifecycleSelfTest/README.md) | 诊断：停止后再启动 | BLE 目标；115200 串口 | 自动执行生命周期，不是常规业务模板 |
| [HardwareSelfTest](HardwareSelfTest/README.md) | 诊断：ESP32 集成/堆检查 | ESP32；部分步骤需 App 与订阅 | 回调打印，不驱动实际扬声器/屏幕；不等于物理外设自检 |

CoC 指 BLE 的 L2CAP 面向连接通道。控制/VOICE 的成功不能证明 App 已打开 AUDIO/IMAGE/TTS/OTA 对应的通道；READY 只表示控制通知可用。本库统一启用两个 CoC 通道：AUDIO/TTS/OTA 共用 PSM 0x0081，IMAGE 使用 PSM 0x0082。详见[配置与兼容说明](../docs/BLE-COC.zh-CN.md)。不要在同一草图中再启动其他 BLE/NimBLE 栈。

BLE 路线：BasicBLE → CachedSensor → SensorActuator / SerialPrompt → 你的真实外设 → 进阶流。

图片路线：CoCDiagnostics → StoredJpegUpload → ImageSequence / CameraSnapshot。前三个无需相机即可验证通道、JPEG 解码和上传生命周期。串口示例的 ESP_OK/完成进度表示本地接受/提交，不是 App 收到图片的确认；实际联调仍需查看 App 的接收与解码结果。

OTA 路线：先在普通 BLE 示例验证 App 连接，再选择有足够容量的双应用 OTA 分区并运行 CoCOtaUpdate。官方 Core 的 `min_spiffs` 保留两个约 1.9 MiB 应用槽，可用于 OTA；Huge APP 等单应用布局不能用于 OTA。实际更新应上传为相同硬件生成的应用 `.bin`，不能使用合并烧录镜像；先计算该文件的 SHA-256，并在 App 的 START 命令中提供长度、目标版本及匹配的 model。

Wi-Fi 路线：BasicWiFi（填写凭据直连）或 WiFiProvisioning（热点配网）→ CloudActivation（配置平台并激活）。Wi-Fi 有 IP、云端在线和 Agent 数据通道 READY 是不同阶段，当前只支持前两者。发生错误按 [排查表](../docs/BEGINNER-GUIDE.zh-CN.md#troubleshooting) 处理，不通过加长 delay 或持续 begin 掩盖问题。
