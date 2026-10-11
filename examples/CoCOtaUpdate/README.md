# CoCOtaUpdate：由 App 发起 BLE CoC 固件升级

[CoCOtaUpdate.ino](CoCOtaUpdate.ino) 显式启用 OTA，在启动时检查分区并准备接收资源，展示升级进度、取消和新固件健康确认。固件通过与 AUDIO/TTS 共用的 **CoC PSM `0x0081`** 接收；本示例不会自行寻找或开始升级。

示例直接使用 Arduino 风格的 `Deotaland.setOtaCallback()`、`otaStatus()`、`otaBusy()`、`otaNeedsReconnect()`、`abortOta()`、`markOtaValid()` 和 `runningVersion()`；与 C API 共用相同 OTA 会话。库还会在准备/START 时检查目标槽不是运行分区，并且 `otadata` 至少为 8 KiB。

## 硬件、分区和 App 前提

- 使用有 BLE 的 ESP32 系列开发板和兼容的官方 Arduino-ESP32 3.x Core。本库自带启用 CoC 的 NimBLE，不需要另外初始化蓝牙栈。
- Flash 分区必须包含 **`ota_0`、`ota_1` 和 `otadata`**，两个应用槽都要能容纳完整固件。此示例会检查三者是否存在，并输出两个槽的实际容量。
- 例如官方 Core 3.3.12 的 ESP32-S3 `min_spiffs` 布局，每个 OTA 应用槽为 1,966,080 字节；仍要核对自己的板型、Flash 容量和固件大小。`Huge APP` 单应用槽布局不能用于本例 OTA。
- App 必须实现 Agent Link OTA 命令和固件传输，订阅控制通知 `0xFFC4` 及 OTA 响应 `0xFFB1`，并打开 CoC `0x0081`。`READY` 不能替代 CoC 连接。
- 不需要麦克风、扬声器或相机。升级期间应停止 AUDIO、TTS 和其他大流量业务。

## 修改配置和准备固件

| 位置 | 默认值 | 应怎样设置 |
| --- | --- | --- |
| `kModel` | `Deotaland-CoC-OTA` | 为同一 PCB/硬件型号保持稳定；App 的 START 必须指定匹配的型号 |
| `kFirmware` | `1.0.0` | 改成当前发布固件的实际版本；新版本再次修改 |
| `config.device_name` | `Deotaland-CoC-OTA` | 按需调整 App 扫描时看到的名称 |
| IDE 的分区方案 | 由用户选择 | 初次 USB 上传就选择满足双槽要求的布局 |

`kFirmware` 写入协议的 `firmware_rev`，不会修改 ESP-IDF 应用元数据。串口 `revision` 和 `running` 分别来自对外上报的设备固件版本与运行镜像元数据，可能不同。

升级文件必须是为**同一芯片和 PCB**构建的应用 `.bin`，不能使用合并烧录镜像、bootloader 或分区表。修改新固件版本后，在 Arduino IDE 导出已编译的二进制文件，选择应用镜像；核对其大小小于目标槽容量。

App 的 START 信息应包含匹配的 `model`、目标版本、应用 `.bin` 的准确字节数和 SHA-256。本示例不提供 App 上传器，也不支持用串口命令代替 OTA START。

## 上传和升级步骤

1. 在 Arduino IDE 打开本目录 `.ino`，按硬件修改型号、版本和板卡设置，选择正确 OTA 分区方案。
2. 初次用 USB 上传。打开 **115200 波特率**串口监视器，确认输出两个 OTA 槽容量，并且 `begin=ESP_OK`、`prepare=ESP_OK`、`supported=1`。
3. 在 App 连接 `Deotaland-CoC-OTA`，完成控制/OTA 响应订阅并打开 `0x0081`。
4. 在 App 选择准备好的新应用 `.bin`，以准确的型号、目标版本、字节数和 SHA-256 发起升级。
5. 观察串口接收和校验状态。成功终态被本地传输后端接受后，后台通常约 2 秒重启；持续运行 `loop()` 中的轮询直至重启。
6. 重连后查看新版本并完成板级启动、外设和业务自检；需要引导回滚健康确认时，再执行 `v`。

## 串口命令

命令只识别下列小写字母。

| 命令 | 行为 |
| --- | --- |
| `?` | 输出型号、协议版本、运行镜像版本、支持情况、控制状态和 OTA 状态 |
| `x` | 请求协作式取消升级；已经进入的 Flash 擦写调用会先返回 |
| `v` | OTA 不忙时调用 `Deotaland.markOtaValid()`，手动确认当前固件健康 |

`v` 不执行硬件自检、不开始升级，也不触发重启；应在实际健康检查通过后使用。它用于启用了引导回滚的相应固件状态，当前构建不支持时可能返回错误。

## 怎样读进度

串口输出 `OTA state=... received=.../... error=...`，状态编号如下：

| 编号 | 状态 | 含义 |
| --- | --- | --- |
| 0 | IDLE | 尚未开始 |
| 1 | RECEIVING | 正在接收/写入应用镜像 |
| 2 | VERIFYING | 接收结束，正在验证镜像 |
| 3 | SUCCESS | SHA-256 和镜像检查通过，已设置启动分区 |
| 4 | FAILED | 升级失败或被取消，结合错误码诊断 |

`received_bytes` 只是接收进度；收到全部字节仍不等于校验成功。串口终态及响应的本地接受，也不代表 App 已收到该响应。

## 失败处理和限制

若缺少分区，启动会提示 `This example needs ota_0, ota_1 and otadata`。应调整分区后重新 USB 上传；仅在 App 选择更小的固件不能补出缺失分区。`prepare` 失败也可能来自内部堆内存不足或不支持的后端。

已开始的升级被取消、失败或断连后，**重新连接再发起下一次 OTA 或 AUDIO/TTS 传输**，避免残留固件字节混入共享通道。取消不会立即中断正在执行的 Flash 操作。

`prepareOta()` 提前创建接收缓冲、队列和后台任务，资源保留到设备重启；受支持的 IDF 构建优先将 32 KiB 接收环放入 PSRAM，不可用时回退内部 RAM。它不能消除 Flash 操作对其他任务的影响，也不提供硬实时保证。

若需要只允许升级到更高版本，可对整个库定义 `AGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1`，默认关闭。它按运行镜像元数据的数字版本比较，`kFirmware` 只是协议标签，不能代替构建时的镜像版本。完整格式及 Arduino CLI 参数见 [API 参考](../../docs/REFERENCE.zh-CN.md#ota)。

分区、供电、升级镜像、重启和恢复流程仍需在实际 PCB 与 App 上验证。编译通过不能替代真实 OTA 和健康检查。

继续阅读：[API 参考中的 OTA 说明](../../docs/REFERENCE.zh-CN.md)、[BLE CoC 说明](../../docs/BLE-COC.zh-CN.md)、[全部示例](../README.zh-CN.md)。
