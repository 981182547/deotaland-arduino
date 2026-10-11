# Agent_link-main 对照与 Arduino 版本完善记录

核对日期：2026-10-11；适用库版本：**0.8.1**。对照对象是本工作区的 `Agent_link-main/firmware/components/agent_link` 和 `firmware/boards` 实际源码。原项目部分 README 的状态表较旧，以下结论以代码为准。

## 功能覆盖

| 功能 | Agent_link-main 实际实现 | Arduino 库当前情况 |
| --- | --- | --- |
| BLE 控制与设备信息 | FFC0/FFC1/FFC4、Battery、DIS、共享设备 UUID、配对绑定 | 已移植；增加轮询预算、队列统计和断线隔离 |
| VOICE 语音上行 | FFA0/FFA1 GATT Notify，PCM16 | 已移植；固定发送环、拥塞重试和预热；旧协议没有可告知对端截断的 END 标记 |
| AUDIO / ASR 上行 | CoC PSM 0x0081，设备上传 PCM | 已移植；[AiotBasicAgent](../examples/AiotBasicAgent/README.md) 使用真实 PDM 麦克风，也可上传 SD 录音解码后的 PCM |
| TTS 音频下行 | PCM16 / IMA-ADPCM 解码和流控 | 已移植；增加可部分接收的播放接口、真实水位统计和回复状态查询 |
| IMAGE 图片上行 | PSM 0x0082；JPEG / RGB565；0x54 / 0x55 开始、结束事件 | 已移植；[图片上传器](IMAGE-UPLOAD.zh-CN.md) 支持超过发送环容量的照片、分块重试、取消和断线清理 |
| 通用 I/O | 清单、分页、执行器、读数、自动 LED / 屏幕 / 震动端点 | 已移植；增加类型化缓存读数与离线最新值合并；本次补严格 UTF-8 校验 |
| SD 录音与列表 | Korvo 板有 PCM / IMA-ADPCM WAV 和 0x04 分页 | AIOT-Basic 示例有后台录音、分页、本地回放和 PCM 上传；本次补可选 ADPCM 存储及读取解码 |
| BLE OTA | 型号检查、SHA-256、Flash 写入、取消、重启、健康确认 | 已移植；默认须显式启用；本次补分区前置检查、Arduino OTA 方法及可选严格版本检查 |
| WiFi 配网 | STA / SoftAP、DNS / HTTP 配网页、凭据保存 | 已移植；验证连接后保存、异步扫描、重连与错误状态 |
| 平台生命周期 | claim / bind / auth / heartbeat、凭据吊销后重新申领 | 已移植；本次修复断网或停止时丢失刚返回的一次性绑定密钥的问题 |
| WiFi 媒体、VIDEO、FILE | 仍是占位或 NOT_SUPPORTED；没有完整运行协议 | 仍不支持。需要先明确平台/App 数据协议，不能通过库内增加空实现完成业务 |
| Agent 列表 / 选择上报、图片和视频下行 | 有接口声明，未完整分发 | 保留接口及不支持状态；不是本次 Arduino 移植遗漏 |
| 各板专用硬件和 UI | ES8311、触摸、温湿度、气压、电量、工牌 UI 等按板实现 | 尚未逐板移植。AIOT-Basic 驱动和接线只适用于对应硬件；通用 SDK 回调可接用户自己的驱动 |

BLE 内置后端随库编译 NimBLE-Arduino 2.5.1，默认两路 CoC；无需改 Arduino Core。两路是同时连接数量上限，OTA 与 AUDIO/TTS 共用 0x0081。详细约定见 [CoC 说明](BLE-COC.zh-CN.md) 和 [API 参考](REFERENCE.zh-CN.md)。

原版核对入口：`firmware/components/agent_link/src/agent_link.cpp`、`transport_ble.cpp`、`transport_wifi.cpp`、`ota_service.cpp`，以及 `firmware/boards/korvo/` 的录音与 ADPCM 驱动。这些路径属于单独的 Agent_link-main 工程，不随 Arduino 安装包分发。

## 本次修复与优化

### 回复开始、结束重发

App 对 `0x05 VoiceReply` 的确切重发此前可能清空库内待播放 PCM，或重复调用 `on_audio_end`。现在对最近一次成功请求的 sequence 和完整 payload 做连续重试去重，仍返回应答。遇到另一个命令、连接世代变化、停止或重新初始化时清除记录。

这是音频控制请求的局部去重，不是所有执行器命令的通用事务缓存。执行器仍应使用明确目标值，例如“设为关闭”；不要依赖重复“切换一次”只执行一次。同一个 session ID 配上新 sequence 仍能开始新回复。

### 字符串值验证

`AGENT_VAL_STR` 读数和执行器参数现在拒绝内嵌 NUL、无效或截断的 UTF-8，包括过长编码、代理区字符和超过 U+10FFFF 的值。有效中文及空串仍可使用。这样线上的完整值不会在 C 字符串回调中被悄悄截断。按 UTF-8 字节计算容量，详见 [读数与类型](REFERENCE.zh-CN.md)。

### 绑定密钥持久化

平台返回的一次性 `auth_key` 此前会在同时断网使请求过期时被丢弃，随后重新 Claim 可能遇到 AlreadyBound。现在成功绑定立即在同一后台任务尝试保存；普通网络断开或停止不会视为解绑。保存失败则保留该密钥继续重试；显式 `forget()` 仍拒绝旧回复或清除已存密钥。

还修复了 `forget()` 返回成功后紧接 `stop()` 会丢失解绑请求的竞态。已接受请求保留至 NVS 清除成功，worker 停止前尝试一次；失败时保留错误和未完成意图，下次启动先清除再加载或认证旧密钥。未完成意图保存在 RAM，不保证断电后恢复；停止不会因存储故障无限等待。

这不提供断电后的无限保证：如果 NVS 写入一直失败且设备重启，未保存的密钥仍会丢失，应依据错误状态处理存储故障。

### OTA 分区、内存和 Arduino 接口

库在准备和接受升级前检查更新槽与当前运行分区不同，并且存在至少 8 KiB 的 `otadata`。单应用槽不会因 `esp_ota_get_next_update_partition()` 返回运行槽而被误认成可升级布局；镜像大小仍必须放入目标槽。

Arduino 接口增加 `setOtaCallback()`、`otaStatus()`、`otaBusy()`、`otaSupported()`、`otaNeedsReconnect()`、`abortOta()`、`markOtaValid()` 和 `runningVersion()`。[CoCOtaUpdate](../examples/CoCOtaUpdate/README.md) 演示这些方法。进度回调继续在 `poll()` 执行，Flash 操作由后台任务执行。

受支持的 IDF 构建优先将 OTA 的 32 KiB 接收环放入 PSRAM，失败时回退内部 RAM；Flash 写缓冲和任务栈保留在内部 RAM。`prepareOta()` 可在采集开始前预热，不能消除 Flash 擦写对其他任务的影响。

全局定义 `AGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1` 可拒绝相同或更旧的数字版本，默认关闭以保持兼容。比较运行镜像的 `esp_app_desc.version`，不是 `config.firmware_rev`；后者是对外上报的设备固件版本字符串，不能替代运行镜像描述符。版本由 1～3 个 uint32 数字段组成，非空的预发行/构建后缀不参与先后排序，因此这不是完整 SemVer 预发行排序器。

### 可选 ADPCM 录音

[AiotBasicAgent](../examples/AiotBasicAgent/README.md) 的 `AIOT_SD_ADPCM` 默认 `0`，保持原来的无损 PCM16 WAV；设为 `1` 使用原项目同类的 IMA-ADPCM WAV。压缩有损，以 256 字节存储 505 个样本，长录音的数据部分约为 PCM16 的四分之一，文件头和尾块补齐另计。

停止时补齐最后一块，`fact` 记录真实样本数；读取时去除补齐样本并解码回 PCM16。`y` 回放和 `u` 上传接口不变，网络 AUDIO 仍发送 PCM，不能直接把 WAV 头或 ADPCM 文件字节写到 RAW 音频流。SD 的后台缓冲、部分文件保护和 GPIO35 冲突检查继续有效。

修复完成录音改名失败的问题：此前先创建目标空文件，再调用 rename，而 [FatFs 的重命名规则](https://elm-chan.org/fsw/ff/doc/rename.html) 不覆盖已有目标，导致正常保存也失败。现在直接使用其不覆盖语义；目标冲突保留已有文件及 partial。同步关闭后用原子提交点确定取消结果，避免迟到取消产生状态不一致。

挂载失败时退出并释放后台任务、PSRAM 缓冲及目录缓存；读取失败后的旧清理也不会清除新请求的占用状态。新增的 WAV 解析覆盖 ADPCM 样本数、块顺序、奇数长度填充及边界检查。

### 预留 Agent 选择接口

`selectAgent()` 在连接 READY 后明确返回 `ESP_ERR_NOT_SUPPORTED`，不再错误提示“尚未就绪”；无效参数及离线调用仍返回相应错误。原项目没有完成选择上报协议，库不会发送猜测的事件。

## 仍需后续工作的范围

- 若要在 ES8311、Korvo 触摸屏或工牌板上直接运行，需要新增对应板的引脚、驱动和示例。不能把这些不同硬件的功能全部套到 AIOT-Basic 上。
- 如需中文屏幕字体，应按目标显示库及字符范围加入字库；当前 AIOT-Basic 内置 ASCII 字体，原项目也未提供通用完整中文字库。
- 如需 WiFi Prompt、音视频、通用 FILE 或 Agent 选择，需补齐并验证 App/平台协议；当前不宣称这些预留接口可用。
- 无线重连、手机 CoC 兼容、音频连续性、图像颜色、SD 拔卡/断电恢复和 OTA 重启/回滚仍需实际开发板与 App 联调。主机回归和 Core 编译验证覆盖代码与接口边界，不能证明这些物理效果。

## 验证

| 检查 | 范围与结果 |
| --- | --- |
| 可移植核心 / Arduino 封装 | 实际生产代码 C++11 编译，139 项断言通过 |
| 云绑定与解绑 | 实际 Session 的 12 场景 / 62 项断言、worker 的 7 场景 / 101 项断言通过；HTTP、NVS、调度边界使用假实现 |
| OTA | 真实服务覆盖默认与严格版本检查、PSRAM 及内部 RAM 回退、IDF 5.0 与静态分配禁用、非 ESP32 平台；全部通过 |
| SD 录音 | 实际 worker / codec 的 PCM、ADPCM、OPI 配置共 77 用例及原有 26 条回归通过；平台和 FatFS 边界使用假实现 |
| Arduino Core 3.3.12 | CoCOtaUpdate：S3 QSPI / C3 严格版本；AiotBasicAgent：QSPI PCM / QSPI ADPCM / WiFi / OPI，6 组编译链接通过且零编译器警告 |

验证工具和日志位于源码工作区的 `.build`，不会恢复库内 `tests`、`extras`，也不随安装包分发。每个示例的运行条件见 [示例索引](../examples/README.zh-CN.md)。这些检查没有烧录实物或连接真实 App。
