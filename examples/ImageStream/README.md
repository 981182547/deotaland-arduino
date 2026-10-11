# ImageStream：用底层流 API 发送 2×2 测试图

[ImageStream.ino](ImageStream.ino) 演示 `openStream()` → `writeStream()` → `closeStream()` 的调用顺序和拥塞重试。它发送内置的 2×2 大端 RGB565 测试图，像素顺序为红、绿、蓝、白，共 8 字节。

## 硬件和 App 前提

- 使用有 BLE 的 ESP32 系列开发板，无需相机、PSRAM 或显示屏；ESP32-S2 没有 BLE，不适用。
- 安装本库和兼容的官方 Arduino-ESP32 3.x Core；本库自带启用 CoC 的 NimBLE，不需要额外初始化蓝牙栈。
- App 需支持 IMAGE 和大端 RGB565，订阅控制通知 `0xFFC4`，独立打开 **IMAGE CoC PSM `0x0082`**。只有控制 `READY` 还不能传图。

## 配置和上传

默认测试无需修改引脚或网络配置。`pixels[]` 是固定的测试像素，`opts.width/height` 为 2，`opts.total_bytes` 为 8；零初始化的编码为 RAW。

1. 在 Arduino IDE 打开本目录 `.ino`，选择真实开发板和能容纳固件的应用分区，USB 上传。
2. 打开 **115200 波特率**串口监视器，查看启动和 IMAGE 支持诊断。
3. 用配套 App 连接 `Image-Sample`，订阅控制通知并打开 IMAGE 通道。
4. 通道可用后程序自动发送一次测试图；无需输入串口启动命令。
5. 在 App 检查收到的 2×2 色块。要再次发送，断开并重新连接，再打开通道；也可重启设备。

## 串口和 App 操作

| 操作 | 行为 |
| --- | --- |
| 串口 `?` | 请求重看启动、控制状态和 IMAGE 支持诊断，报告最多每 500ms 一次 |
| App 连接并打开 IMAGE | 自动启动本连接的一次传图 |
| App 断连并重连 | 丢弃旧句柄，允许新连接重新传图 |

程序每 10 秒自动输出诊断。没有串口启动、取消命令，也没有注册 `camera0` 端点；声明相机能力只用于图片协议。

## 流程与预期现象

| 阶段 | 操作 |
| --- | --- |
| Open | 创建 IMAGE 流并声明宽、高和总字节数 |
| Write | 写入同一个 8 字节像素块 |
| Close | 提交成功 END，然后停止本连接的发送任务 |
| Abort | 永久写入错误时尝试提交失败 END |

每次操作尝试间隔至少 100ms。`ESP_ERR_NO_MEM` 或 `ESP_ERR_TIMEOUT` 时保留原操作重试；打开时的 `ESP_ERR_INVALID_STATE` 会等待 App 的 IMAGE 通道，不设整体等待超时。

永久错误会输出 `image transfer: ...`，随后中止或停止本连接的任务，直到重新连接。成功时没有专门的“App 已收图”日志，应从 App 检查色块；周期诊断也不包含发送进度。

## 扩展和限制

RAW 数据必须满足宽×高×2 字节，并采用大端 RGB565。更换为 JPEG 时，必须使用真实 JPEG 字节、`AGENT_ENC_JPEG`、实际尺寸及准确总长度。

此示例只有一个很小的写入块。较大 JPEG 需要分块、保存偏移和管理源缓冲生命周期，可参考 [StoredJpegUpload](../StoredJpegUpload/StoredJpegUpload.ino) 或 [CameraSnapshot](../CameraSnapshot/CameraSnapshot.ino)。

`writeStream()` / `closeStream()` 的 `ESP_OK` 表示本地接受，后台发送可能尚未完成，**不代表 App 收到或成功解码**。句柄只属于当前连接，关闭或断连后不可继续使用。

继续阅读：[图片上传指南](../../docs/IMAGE-UPLOAD.zh-CN.md)、[BLE CoC 说明](../../docs/BLE-COC.zh-CN.md)、[全部示例](../README.zh-CN.md)。
