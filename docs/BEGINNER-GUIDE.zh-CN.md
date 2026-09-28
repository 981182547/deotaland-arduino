# 中文入门指南

适用库版本：**0.6.1**；核对日期：2026-09-28。内容已对照当前源码和示例；编译与实测边界见 [0.6.1 验证记录](../tests/VALIDATION-0.6.1.md)。文中的 Core 版本是已验证基线，不表示只有该版本可用，也不表示它是当前最新 Core。

BLE 路线先看到 READY 和一条模拟读数，再接入真实传感器；Wi-Fi 路线先连接路由器取得 IP，再配置云端激活。你可以直接从第 6 节开始使用 Wi-Fi，不需要先做 BLE 配对。

## 1. 准备与安装

准备可传数据的 USB 线和 Arduino IDE，再按路线选择开发板：BLE 练习需要有 BLE/NimBLE 支持的 ESP32 及兼容 Agent Link 的 App；Wi-Fi 练习需要有 Wi-Fi 的 ESP32 和可连接的路由器，BasicWiFi 不需要 BLE 或 Agent Link App。本文的编译基线是 ESP32-S3 + Arduino-ESP32 3.3.11。库的便携核心还能用于 STM32 自定义传输，但那需要自行实现桥接，不适合作为第一课。

在开发板管理器安装 Espressif 的 `esp32` Core。库使用 Core 自带 NimBLE C 接口，不需要再装 `NimBLE-Arduino`；额外安装它不能为 Core 打开 CoC。不要把 `extras/idf-reference` 当作 Arduino 库安装。

用“Sketch → Include Library → Add .ZIP Library”选择安装包。手动安装时目录应是 `Sketchbook/libraries/deotaland-arduino/src/Deotaland.h`，而不是多套一层同名目录。重开 IDE，从 File → Examples 找到本库。[官方安装说明](https://support.arduino.cc/hc/en-us/articles/5145457742236-Install-libraries-in-the-Arduino-IDE)

安装后确认 `library.properties` 中的 `version=0.6.1`。若找不到 `updateFloat` 或 `status().wifi_connected`，查看 IDE 详细编译输出采用的库路径，排除旧副本。`config.firmware_rev` 是你自己的设备固件版本，不必跟随库版本填写。

选择与你的实际芯片一致的板型、端口、Flash 大小和分区。完整构建含 TLS，默认的小应用分区可能放不下；本次 S3 编译使用 `min_spiffs`，应用槽 1,966,080 字节并保留双槽。按实际 Flash 选择，不能为了通过编译虚填容量。[Espressif 工具菜单](https://docs.espressif.com/projects/arduino-esp32/en/latest/guides/tools_menu.html)

若用 S3 的原生 USB 口看 `Serial`，按板卡配置启用 `USB CDC On Boot`；若用 CH340/CP210x 等转串口芯片，应选对应端口，不能套用原生 USB 设置。打开监视器选 115200。原生 USB 首次烧录后可能需要复位并重新选择端口。[Espressif USB CDC 指南](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html)

## 2. 第一课：BasicBLE

上传 [BasicBLE](../examples/BasicBLE/BasicBLE.ino)。无需连接其他硬件；在 App 内搜索 `Deotaland-ESP32` 并连接。串口会周期性显示状态，发送 `?` 也可主动查询。

| 看到的状态 | 含义 | 下一步 |
| --- | --- | --- |
| begin 报错 | 驱动尚未启动成功 | 查错误名和排查表，不继续上报 |
| DISCONNECTED | 尚未建立当前可用会话 | 在 App 内扫描、连接 |
| CONNECTED | 链路建立，但事件通道未就绪 | App 需订阅事件特征 0xFFC4 |
| READY | 控制/事件通道就绪 | 继续传感器练习 |

期望日志形如 `连接状态: READY`。UUID 每台设备不同；首次持久化后才会可用。模拟电量固定为 80%，并非每次采样都发包：库会去重，重连后会重新同步。普通扫描工具订阅只够做 GATT 调试，不会自动完成 Agent 业务交互。

## 3. 第二课：CachedSensor

上传 [CachedSensor](../examples/CachedSensor/CachedSensor.ino)，连接 `Deotaland-Sensor`。程序每秒生成 20.0～20.9°C 的循环模拟值，每 5 秒打印一次日志；App 中的端点 ID 是 `temp0`。

一次传感器接入包含四步：

1. 全局创建 `agent_link_io_desc_t`，填写短且唯一的 id、IN 方向、语义 kind、数据类型和单位。
2. `setup()` 中先 `registerIo`，检查返回值，再 `begin`。
3. `loop()` 每轮调用 `poll()`，用 `millis()` 决定何时采样。
4. 调用 `updateFloat("temp0", 实际温度)`，库会复制、缓存并稍后发送。

`updateFloat` 要配 F32，整数用 `updateInt32` 或 `updateUInt16`，开关用 `updateBool`，文本用 `updateText`；完整类型对应见 [API 概览](REFERENCE.zh-CN.md#api-overview)。不要通过强制转换去“消除”类型不匹配；先检查端点声明。`double` 应确认精度与范围后转换为 `float`，数组和其他类型仍用原始接口。

`updateXXX` 返回 `ESP_OK` 表示已接受到本地缓存，不代表 App 已收到。文本按 UTF-8 字节计数，默认最多 125 字节，不含结尾 NUL；`updateText(id, "")` 可清空文本，`nullptr` 会被拒绝。

先断开 App，等待数秒后重连：应同步最新温度，不会把离线期间的每个旧样本逐个补发。要保存每次采样或报警事件，需要应用自己的记录/确认机制；最新值缓存不替代日志。

接真实硬件时，只需先替换 `readTemperatureC()` 并初始化对应驱动。采样失败时不要上报假的 0；检测驱动错误，保留上次有效读数，并通过独立状态端点说明故障。NaN/Inf 会被库拒绝，缓存不被污染。传感器慢速转换应拆成“发起测量”和“读取结果”，让 loop 继续运行。

## 4. 第三课：SensorActuator

上传 [SensorActuator](../examples/SensorActuator/SensorActuator.ino)，连接 `Deotaland-IO`。将 `led0` 设为 true/false，串口输出 ON/OFF，`enabled0` 回传软件状态，`uptime0` 每秒更新运行时间。

默认不操作任何 GPIO。确认普通 LED 的引脚与有效电平后，修改 `kLedPin` 和 `kLedActiveLow`；可编程 RGB 地址灯须使用自己的驱动。`led0` 在此例代表逻辑开关，不代表已测得真实灯亮。应用若需要证明硬件动作完成，应增加实际状态采样/故障读数。

回调里只接收目标值；`loop()` 应用它。命令用“设为开启”，不要用“切换一次”：重发相同命令仍会得到相同状态。BOOL 参数是一个 0/1 字节，STR 是带长度的 UTF-8，不能未经复制和补 NUL 就当作 C 字符串使用。

描述符、描述字符串和 ctx 需要持续有效；用全局/static 对象。`begin` 会复制配置字符串及输出回调表，但不会复制 I/O 描述符，也不会延长 ctx 或自定义传输对象的寿命。

## 5. 第四课：SerialPrompt

上传 [SerialPrompt](../examples/SerialPrompt/SerialPrompt.ino)，在 App 连接 `Deotaland-Prompt` 后，串口发送 `p`。程序发送“你好”，可附加换行；`?` 显示连接状态和允许的提示词字节数。

提示词按 UTF-8 字节计数。默认 BLE MTU 下通常最多 14 字节，中文通常每字 3 字节。`maxPromptBytes()` 反映当前协商容量；长句需要先协商更大 MTU 或缩短文本，不能把一个提示词拆成多次 pushPrompt 假定平台会自动合并。

示例每 100ms 尝试一次，只在拥塞时重试；断线或等待超过 5 秒取消。它不会在 `while` 中反复发送，也不会把旧提示词留到新连接。发送成功意味着本地接受，实际 Agent 回复需要 App 和平台业务链路正常。

## 6. Wi-Fi、语音和图片什么时候开始

**填写账号密码直接连接：** 打开 [BasicWiFi](../examples/BasicWiFi/BasicWiFi.ino)，修改顶部的 `kSsid` 和 `kPassword`，上传后打开 115200 串口。看到 `Wi-Fi 已连接，IP: ...` 即表示已连上路由器；串口 `?` 可随时查询。示例使用下面两个配置字段，不要同时调用其他库的 `WiFi.begin()`：

```cpp
agent_wifi_config_t wifi = {"你的 Wi-Fi 名称", "你的 Wi-Fi 密码"};
agent_link_config_t config = {};
config.device_name = "Deotaland-WiFi";
config.transport = AGENT_TRANSPORT_WIFI;
config.wifi = &wifi;
// setup 中调用并检查 Deotaland.begin(config)，loop 中持续 Deotaland.poll()。
```

`begin` 成功表示后台任务已启动，不代表已获取 IP。0.6.1 起，无需配置云平台也能通过 `status()` 区分配网、连接中和已联网。持续 `poll()` 后读取：

```cpp
const auto status = Deotaland.status();
if (status.wifi_connected) Serial.println(status.wifi_ip); // 路由器分配的 IP
if (status.wifi_portal_active) Serial.println(status.hint); // 实际配网热点提示
```

无平台配置、已获取 IP 且配网门户已关闭时，阶段为 `AGENT_LINK_PHASE_NETWORK_READY`。这是网络已连接，不是 Agent 数据通道 READY；仅有 IP 也不保证互联网或云端可达。断网后库会清空 IP，BasicWiFi 无需自行注册 ESP-IDF 事件监听器。

| 看到的阶段/字段 | 含义与操作 |
| --- | --- |
| CONNECTING / Joining WiFi | 尚未获取 IP，持续 poll，检查 SSID、密码和网络 |
| SETUP / wifi_portal_active=true | 配网门户已启动，按 hint 中的热点名称连接 |
| NETWORK_READY / WiFi connected | 本例连接路由器的目标已完成，可读取 wifi_ip |
| CONNECTED / Platform online | 已配置平台且鉴权在线；仍没有 Wi-Fi 控制/媒体数据通道 |
| BLOCKED | 查看 title/hint/detail，区分后台 Wi-Fi 服务错误与平台错误 |

配网成功后的短暂收尾期间，`wifi_connected` 和 `wifi_portal_active` 可以同时为 true，阶段仍为 SETUP；这是两个独立状态。BasicWiFi 每 5 秒重复诊断，`?` 可主动查询，连续查询也会限频。

[WiFiProvisioning](../examples/WiFiProvisioning/WiFiProvisioning.ino) 用于配网，无保存凭据时打开 `Deotaland-WiFi-XXXX` 热点。手机若提示无互联网，保持此连接；门户没弹出时打开 `http://192.168.4.1/`，若网关被板级配置修改则以实际网关为准。不要同时让 `WiFi.begin()` 和本库争用同一个驱动。

**换网时注意凭据优先级：** 草图中非空的 `config.wifi->ssid` 每次启动都优先于 NVS 保存值。用配网页换网后，若草图仍写着旧 SSID，重启会再次尝试旧网络；更新草图，或改用不预设凭据的 WiFiProvisioning。开放网络的密码填 `""`，不要保留 `YOUR_WIFI_PASSWORD` 占位符。

[CloudActivation](../examples/CloudActivation/CloudActivation.ino) 需要你部署/使用的平台参数。示例域名和 product ID 是占位符。激活成功也不会让 Wi-Fi 的 `isReady()` 变 true：当前只完成激活、绑定、鉴权和心跳，尚无控制/音视频数据通道。

CloudActivation 默认使用已保存凭据或热点配网；若希望它也预设 SSID/密码，把上面 `wifi` 和 `config.wifi = &wifi` 加到其 setup 的 begin 之前。不要假设 BasicWiFi 中写在固件里的凭据一定已持久化到 NVS。

语音/图片示例放在控制链路练习之后。VOICE 使用 GATT，但测试程序产生的是合成音；AUDIO、IMAGE、TTS 和 OTA 要求启用 CoC 的 Core 以及相应 App 通道。VoicePTT、RealtimeASR、ImageStream、BufferedPlayback 会在启动后检查所需能力，每 10 秒或输入 `?` 可重新查看诊断。库不会替你选择 I2S、摄像头或扬声器引脚。每个进阶示例的替换点见 [索引](../examples/README.zh-CN.md)。

## 7. 不容易出错的 loop

持续 `poll()`，用无符号时间差 `uint32_t(now - lastTime)` 安排采样。失败后的重试必须留到后续 loop，不能阻塞等待 READY；大量串口打印也会影响通信。

所有业务接口在同一 loop 线程调用，不在中断里调用。回调不能执行生命周期操作；需要停止或重启时，回调只设置标志，loop 看到标志后调用 `end()`，等 `canBegin()` 为 true 再 `begin()`。端点只注册一次：end 保留它们，reset 才清除。

[ResponsiveLoop](../examples/ResponsiveLoop/ResponsiveLoop.ino) 演示限时执行动作和低压力发送：拥塞重试至少间隔 100ms，成功或永久错误后等待下一个上报周期。`isRetryable()` 仅识别 NO_MEM/TIMEOUT，不代表初始化内存不足也应无限重试。

<a id="troubleshooting"></a>
## 8. 常见问题与当前边界

| 现象 | 先检查与处理 |
| --- | --- |
| 找不到头文件、新方法或 Wi-Fi 状态字段 | 检查库目录结构、实际编译路径及 0.6.1 版本 |
| Sketch too big | 按真实 Flash 容量选择足够大的应用分区，参考第 1 节 |
| 上传后串口空白 | 检查板卡对应的 USB/CDC、端口、115200；打开串口后复位或等待周期诊断 |
| begin / streamStatus 返回 NOT_SUPPORTED | 检查芯片、Core、所选传输与 CoC 支持；不能靠重试补齐能力 |
| BLE 一直 CONNECTED | App 需订阅事件特征 0xFFC4；READY 后媒体仍可能等待各自通道 |
| updateXXX 成功但没有读数 | 检查端点 ID/方向/类型、持续 poll、App 的上报策略和当前包容量 |
| 提示词/读数过长 | 按 UTF-8 字节检查 maxPromptBytes 与 MTU；读数还占用 ID 字节数加 2 字节 |
| Wi-Fi 不知道是否连上或找不到热点 | 查 wifi_connected、wifi_ip、wifi_portal_active 和 title/hint，不用 isReady 判断路由器连接 |
| Wi-Fi 在线但 isReady=false | 0.6.1 尚无 Wi-Fi 控制/媒体数据通道；当前支持联网、配网与平台激活/鉴权/心跳 |
| 没声音或识别为噪声 | VoicePTT/RealtimeASR 默认是合成测试音；BufferedPlayback 无扬声器驱动，需按真实硬件接入 |
| 重连后旧 stream 句柄报错 / duplicate id | 重新打开当前会话的流；I/O 端点不要每次连接重复注册，变更配置时才 reset |

先检查本次 API 返回值，再看 `state()/isReady()` 或 Wi-Fi 网络字段。`stats().last_error` 是最近一次记录的历史错误，成功不会自动清零；它不代表当前连接必然失败。不要在日志中输出 Wi-Fi 密码、平台密钥或完整私密提示词。

库中的状态、防错和示例诊断已经改进；Wi-Fi 业务数据通道、指定板卡驱动及真实无线、CoC、音视频、OTA、长期稳定性仍待后续实现或实测。[0.6.1 验证记录](../tests/VALIDATION-0.6.1.md) 记录 8 个 S3 示例及 H2/STM32 两个目标的编译结果，不能视为这些硬件功能已通过验收。API 的预算、内存、媒体流、OTA 和自定义传输约定见 [进阶参考](REFERENCE.zh-CN.md)。
