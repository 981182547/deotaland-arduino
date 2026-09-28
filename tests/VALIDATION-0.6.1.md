# 0.6.1 验证记录

日期：2026-09-28。验证本轮 Wi-Fi 状态、网络事件失效保护和示例修复；未上传设备。最终源码与示例校验清单见 [SOURCE-SHA256-0.6.1.json](SOURCE-SHA256-0.6.1.json)，接口和剩余功能边界见 [API 参考](../docs/REFERENCE.zh-CN.md)。

## 主机回归

Windows MSVC，编译生产源码，传输/硬件边界使用测试替代实现。

| 套件 | 本轮结果 | 覆盖范围 |
| --- | --- | --- |
| run-network-optimization-tests.ps1 | 2610 + 18 条断言 | 96 种网络/平台/云状态组合、故障时保留 IP、断网清除旧云信息、旧 GOT_IP 代次拒绝、128 次重连、凭据/会话计时及 worker 唤醒 |
| run-host-tests.ps1 | 13810 checks / 16 组 | 新增公共状态复制、回调时序、IP 字符串截断、end/reset 清空及迟到回调防护；最终以 ASAN 执行，无异常 |
| run-service-tests.ps1 | 9513 checks | PCM/ADPCM、流控、OTA 进度与配网文本 |
| run-audit-tests.ps1 | 151183 checks | 调度、缓存、边界与 10000 轮热路径无堆分配验证 |

各套编译无警告。断言数量包含循环，不代表独立用例数量。网络顺序测试是确定性主机模拟；ESP32 临界区及 FreeRTOS 实际并发仍需硬件验证。本记录仅包含本轮实际执行的相关回归，不代表全部测试矩阵均已重新执行。

日志位于工作区 `.build/fix-0.6.1/`，主机最终日志 `host-tests-asan.log`、`service-tests.log`、`audit-tests.log`。网络测试可从源码目录执行上述脚本复现；ASAN 用进程环境 `CL=/fsanitize=address /Zi` 运行 host 脚本，随后恢复环境。

## Arduino 编译与链接

Arduino CLI 1.5.1、Arduino-ESP32 3.3.11、STM32 Core 3.0.0；使用 `--warnings all`。8 个 S3 相关示例及 2 个跨平台目标通过，零警告。S3 FQBN 为 `esp32:esp32:esp32s3:PartitionScheme=min_spiffs`，其余完整 FQBN、资源和日志路径见 [BUILD-0.6.1.json](BUILD-0.6.1.json)。

| 目标 | 示例 | Flash 字节 | 静态 RAM 字节 | 警告 |
| --- | --- | ---: | ---: | ---: |
| ESP32-S3 | ResponsiveLoop | 1295352 | 95688 | 0 |
| ESP32-S3 | ImageStream | 1311020 | 95672 | 0 |
| ESP32-S3 | RealtimeASR | 1311520 | 95744 | 0 |
| ESP32-S3 | BufferedPlayback | 1310920 | 103872 | 0 |
| ESP32-S3 | VoicePTT | 1311920 | 95744 | 0 |
| ESP32-S3 | BasicWiFi | 1310028 | 95656 | 0 |
| ESP32-S3 | WiFiProvisioning | 1309504 | 95824 | 0 |
| ESP32-S3 | CloudActivation | 1310016 | 95632 | 0 |
| ESP32-H2 | CachedSensor | 738177 | 63572 | 0 |
| STM32 F401RE | CustomTransport | 37940 | 41824 | 0 |

BasicWiFi 在临时副本中仅把两个占位常量替换为虚拟 SSID/密码，覆盖完整连接分支；CloudActivation 的临时副本将默认 product ID 从 0 改为 1，覆盖平台配置分支。没有上传、连接网络或使用真实凭据。最终这三个 Wi-Fi 示例日志在 `final-wifi/`，跨平台在 `cross/`；常规 CloudActivation 占位分支的预检查结果不计入上表。

完整示例矩阵可用 `./tests/run-arduino-matrix.ps1` 重跑；脚本现已自动覆盖 BasicWiFi 和 CloudActivation 的配置分支。此次只重编与变更相关的目标，不作为全部示例的全量验证。

## 安装包

版本为 0.6.1。安装 ZIP 包含源码、16 个示例、中文文档、构建记录和验证说明，不含测试程序、编译产物或 extras。打包后解包核对 67 个源码/示例/元数据 SHA-256、文档链接和禁入文件，并附 ZIP 的 SHA-256 文件。使用 SOURCE-SHA256-0.6.1.json 清单验证本包。

## 尚未验证及待补充

未烧录设备、未操作 App、未测试真实路由器/DHCP、热点配网、云端 TLS/绑定、射频重连、音频硬件、CoC、OTA Flash 写入、掉电恢复或长期压力。所用常规 ESP32 Core 未启用 CoC；编译通过只验证能力检查和禁用分支，不代表 AUDIO/IMAGE/TTS/OTA 已可工作。

Wi-Fi 控制/媒体数据通道仍缺服务端运行协议；指定板卡音频驱动仍缺硬件参数。用户已确认先完成独立修复，后续再补充资料。新增状态与提示没有代替这些功能。状态结构新增字段，应用和库需一起重新编译；旧枚举数值保持不变。
