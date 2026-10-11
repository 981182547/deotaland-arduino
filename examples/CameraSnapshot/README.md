# CameraSnapshot：拍摄真实照片并上传 JPEG

[CameraSnapshot.ino](CameraSnapshot.ino) 接入真实 ESP32 相机，后台采集并编码 JPEG，再通过 `DeotalandImageUpload` 分块上传。它支持 App/Agent、串口和按键触发，默认适配原版 **ESP32-S3 + GC2145 + PSRAM** 相机板。

## 硬件和 App 前提

- 使用有 BLE、受 `esp_camera` 支持的 ESP32 或 ESP32-S3 相机开发板；默认配置需要 PSRAM。ESP32-S2 没有 BLE，不能运行本示例的无线传图流程。
- 安装本库和兼容的官方 Arduino-ESP32 3.x Core；当前编译基线是 3.3.12。本库已经包含启用 CoC 的 NimBLE，无需另外初始化 `BLEDevice` 或 `NimBLEDevice`。
- App 需支持 Agent Link 控制、JPEG 接收，并订阅控制通知 `0xFFC4`、打开 **IMAGE CoC PSM `0x0082`**。`READY` 只表示控制通道就绪。

## 上传前修改配置

先检查 [camera_pins.h](camera_pins.h)。默认引脚不是 ESP32-CAM/OV2640 引脚表，换板必须按实际接线修改。

| 相机信号 | 默认 GPIO | 相机信号 | 默认 GPIO |
| --- | --- | --- | --- |
| XCLK | 15 | SIOD / SIOC | 4 / 5 |
| D7 / D6 | 16 / 17 | D5 / D4 | 18 / 12 |
| D3 / D2 | 10 / 8 | D1 / D0 | 9 / 11 |
| VSYNC / HREF | 6 / 7 | PCLK | 13 |
| PWDN / RESET | -1 / -1，未接 | 拍照按键 | 3，高电平有效 |

| 配置 | 默认值及用途 |
| --- | --- |
| `CAMERA_PIXEL_FORMAT` / `CAMERA_FRAME_SIZE` | RGB565 / 240×240；GC2145 不输出原生 JPEG，软件编码后上传 |
| `CAMERA_FB_COUNT` / `CAMERA_REQUIRE_PSRAM` | 2 / 1；默认双帧缓冲要求检测到 PSRAM |
| `CAMERA_JPEG_QUALITY` | 80；软件 JPEG 质量范围 1～100，值越大质量越高 |
| `CAMERA_SENSOR_JPEG_QUALITY` | 12；仅原生 JPEG 传感器使用，范围 0～63，值越小质量越高 |
| `CAMERA_MASK_TOP_ROWS` | 8；仅对 GC2145 RGB565 顶部异常行使用替换处理 |
| `CAPTURE_BUTTON_PIN` | 3；内部下拉，按下接高电平；改为 -1 可禁用按键 |
| `CAMERA_VFLIP` / `CAMERA_HMIRROR` | 0 / 0；按成像方向调整 |
| `CAMERA_CAPTURE_TIMEOUT_MS` / `CAMERA_UPLOAD_TIMEOUT_MS` | 15000 / 30000；采集及上传等待超时 |
| `CAMERA_AUTO_PROMPT_INTERVAL_MS` | 0，关闭自动提示；改为 10000 可每 10 秒尝试发送提示文本 |

如果换用支持原生 JPEG 的传感器，例如 OV2640，同时修改引脚和 `CAMERA_PIXEL_FORMAT=PIXFORMAT_JPEG`，并确认驱动支持该传感器。自动提示及串口 `p` 使用 `CAMERA_AUTO_PROMPT_TEXT`，可按业务修改。

## 上传和使用步骤

1. 在 Arduino IDE 打开本目录 `.ino`，保持 [CameraCapture.h](CameraCapture.h)、[CameraCapture.cpp](CameraCapture.cpp) 和 `camera_pins.h` 与它同目录。
2. 选择真实开发板、Flash 容量和 PSRAM 类型，并启用 PSRAM；不要把其他板的 Octal/OPI 设置直接套用到自己的硬件。选择能容纳固件的应用分区，例如容量合适的 `min_spiffs`。
3. USB 上传后打开 **115200 波特率**串口监视器。若提示 `camera init` 错误，先检查接线、供电、传感器和 PSRAM。
4. 在配套 App 连接 `Deotaland-Camera`，订阅控制通知并打开 IMAGE 通道，再用下表任一方式触发拍照。
5. 在 App 确认实际收到并能解码照片；串口 `?` 可重看诊断，程序也会每 10 秒报告一次。

## 串口和 App 操作

| 操作 | 行为 |
| --- | --- |
| 串口 `c` / `C` | 请求拍摄一张新照片并上传 |
| 串口 `p` / `P` | 给 Agent 发送拍照分析提示文本；Agent 仍需调用 `camera0=true` 才拍照 |
| 串口 `?` | 输出启动、相机、控制连接诊断 |
| App 的 `camera0=true` | 调用 `camera.capture` 布尔端点，拍照并上传 |
| App 的 `camera0=false` | 不触发拍照，也不是取消命令 |
| GPIO3 按键 | 高电平上升沿触发，默认消抖 30ms；以配置的 GPIO 为准 |

此示例没有串口取消命令。发送提示文本不附带照片，也不会直接调用相机；长提示可能受 App 协商的 ATT MTU 限制。

## 预期现象和限制

采集成功后会看到 `JPEG 240x240, ... bytes: upload started`，随后可能出现 `JPEG queued with END locally; App receipt is not acknowledged here.`。这表示图片和 END 已被本地接受，**不代表 App 已收到、解码或分析完成**。

采集及编码在后台任务执行，库调用留在 `loop()`。同一时间只处理一张照片，忙时重复触发不会形成照片队列；开始下一张前会等待已有 IMAGE 流结束排空。JPEG 缓冲在上传借用结束后释放。

断连会取消旧请求和上传，晚到的旧采集结果会被丢弃，重连后需重新触发。采集超时时允许后台任务安全结束，不会强行中止仍占有帧缓冲的驱动。缺少 IMAGE 通道、拥塞或超时会导致上传失败。

真实相机、供电、PSRAM 和 App 收图仍需在目标硬件上联调；编译通过不能替代这些检查。

继续阅读：[图片上传指南](../../docs/IMAGE-UPLOAD.zh-CN.md)、[BLE CoC 说明](../../docs/BLE-COC.zh-CN.md)、[全部示例](../README.zh-CN.md)。
