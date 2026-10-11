# StoredJpegUpload：上传 Flash 中的真实 JPEG

[StoredJpegUpload.ino](StoredJpegUpload.ino) 用 `DeotalandImageUpload` 分块发送 [sample_jpeg.h](sample_jpeg.h) 中的真实 JPEG，不需要相机、文件系统或 PSRAM。内置内容是原创 CC0 彩色渐变/棋盘测试卡，尺寸 32×24、1172 字节，并非相机照片。

## 硬件和 App 前提

- 使用有 BLE 的 ESP32 系列开发板；ESP32-S2 没有 BLE，不适用。ESP32 上的 `const` JPEG 使用映射 Flash，无需先复制整张图片到 RAM。
- 安装本库和兼容的官方 Arduino-ESP32 3.x Core；本库已包含启用 CoC 的 NimBLE，不要另外初始化蓝牙栈。
- App 需支持 Agent Link JPEG 接收，订阅控制通知 `0xFFC4`，独立打开 **IMAGE CoC PSM `0x0082`**。`READY` 只表示控制通道就绪。

## 配置和上传

默认测试不需要修改任何 GPIO、网络或相机配置。`sample_jpeg.h` 的字节数组、尺寸常量和 `.ino` 中的 JPEG 编码已配套。

1. 在 Arduino IDE 打开本目录 `.ino`，保持 `sample_jpeg.h` 与它同目录。
2. 选择真实开发板和容量足够的应用分区，USB 上传，打开 **115200 波特率**串口监视器。
3. 查看首次报告的 `begin` 和 IMAGE 支持结果；用 App 连接 `Deotaland-StoredJPEG`。
4. App 订阅控制通知、打开 IMAGE CoC 后，输入 `j` 上传一次 JPEG。
5. 在 App 确认收到并能解码 32×24 测试卡。输入 `?` 可查看当前状态，输入 `x` 可取消等待或上传。

## 串口命令

| 命令 | 行为 |
| --- | --- |
| `j` / `J` | 请求上传一张内置 JPEG；需要控制连接已就绪 |
| `x` / `X` | 清除待发送请求，取消当前上传，并继续轮询 END 清理 |
| `?` | 查看 JPEG 信息、控制状态、IMAGE 队列/可写空间、上传器状态和接受字节数 |

报告由启动或 `?` 触发，没有周期报告。忙时再次输入 `j` 不叠加请求。本例没有注册 App 的 `camera0` 端点，上传由串口触发。

## 预期现象和失败处理

正常开始时输出 `JPEG start: ESP_OK`，完成后输出 `Upload finished locally: ESP_OK`。**这些结果仅表示图片和 END 被本地接受，不是 App 收到或解码的 ACK**；后台仍可能排空数据。

如果控制通道已 READY、IMAGE 尚未打开，发送请求会等待最多 30 秒；超时会提示 `Request expired while waiting for IMAGE; send j to try again.`。上传器本身也采用默认 30 秒超时。

断连会取消属于旧连接的请求和上传，不会在重连后自动续传；重新完成连接和通道设置后再次输入 `j`。取消不能撤回已经被本地接受的数据；END 无法清理时可能需要重新连接才能启动新图片。

`?` 中 `accepted` 是本地接受进度，`queued/writable` 是发送队列信息，都不能替代 App 的收图检查。

## 替换自己的 JPEG

1. 用有效 JPEG 的完整字节替换 `kSampleJpeg[]`，同步修改 `kSampleJpegWidth`、`kSampleJpegHeight` 为实际尺寸。
2. 保持 `AGENT_ENC_JPEG`，总长度由 `sizeof(kSampleJpeg)` 自动取得；不要把 RAW 字节仅改名为 JPEG。
3. 确保源数据从 `start()` 到上传器 `busy()` 变为 false 一直有效。本例的全局 `const` 数组天然满足这个生命周期。

上传器每次轮询至多提交一次操作，写入块最多 512 字节，可发送大于发送队列的完整图片。它检查元数据和长度，不验证 JPEG 码流；图片能否正常解码仍需在 App 实测。

继续阅读：[图片上传指南](../../docs/IMAGE-UPLOAD.zh-CN.md)、[BLE CoC 说明](../../docs/BLE-COC.zh-CN.md)、[全部示例](../README.zh-CN.md)。
