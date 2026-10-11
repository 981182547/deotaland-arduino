# VoicePTT：按住发送的语音会话

[源码](VoicePTT.ino)演示按住开始、松开结束、等待排空和断连取消旧句柄。
**默认发送合成测试 PCM，不采集人声；TTS 仅计数并丢弃，不驱动扬声器。**

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- 兼容 Agent Link 语音业务的 App；控制 READY 后还须订阅 VOICE 通知 `0xFFA1`。
- TTS 下行另外需要 App 打开 CoC `PSM 0x0081`；GATT VOICE 上传不依赖 CoC。
- USB/串口和 115200 串口监视器；默认无需按键、麦克风或扬声器接线。
- 先按[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)验证 BasicBLE，再看 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。

## 配置与硬件替换点

设备名称为 `Deotaland-Voice`，声明 MIC、SPEAKER 和 BUTTON 能力。
`setup()` 用 `prepareStreams()` 预分配 VOICE 环和后台工作任务；准备失败时停止语音任务并打印原因。
能力声明不代表已经发现真实麦克风/扬声器。

| 替换点 | 接入要求 |
| --- | --- |
| `makeTestPcm()` | 替换为 16 kHz、有符号 16 位、单声道麦克风 PCM |
| `onAudio()` | 将 TTS PCM 快速复制到自己的播放队列，不能等待播完 |
| `kPttPin` | 默认为 -1；可改为真实按键 GPIO，INPUT_PULLUP，按下接 GND |

本例每约 20 ms 生成 320 个采样，即 640 字节测试音；不包含 I2S、codec 或 DMA 初始化。
按键使用约 20 ms 去抖，不能假定每块板的 BOOT 引脚都适合本例。
若需要有真实水位和部分接收的播放，参考 BufferedPlayback；默认 `onAudio()` 仅累加 `tts_bytes`。
`onAudioEnd()` 将计数清零，它不能证明物理音频已经播放。

## 上传与操作

1. 打开 [VoicePTT.ino](VoicePTT.ino)，按真实板卡设置上传，打开 115200 串口。
2. 查看启动诊断，确认 VOICE 准备成功。
3. 在 App 连接 `Deotaland-Voice`，订阅事件 `0xFFC4` 进入 READY，并订阅 VOICE `0xFFA1`。
4. 串口发送 `p`，等待 `PTT started`；测试音持续产生。
5. 串口发送 `r`，查看 `closeStream(VOICE): ESP_OK`，等待后台排空后再开始下一段。
6. 若接实体按键，也可按住/松开控制；串口状态与实体按键采用“任一按下即发送”。

## 串口命令

| 命令 | 行为 |
| --- | --- |
| `p` / `P` | 模拟按下，保持发送请求直到收到释放命令 |
| `r` / `R` | 模拟释放；实体按键仍按住时不会结束 |
| `?` | 重看控制状态、VOICE 准备结果和 TTS/CoC 能力 |

可附加换行；没有逐包日志。程序也约每 10 秒打印一次诊断，连续 `?` 会限频。
若断连时仍按住按键/保持串口按下，重连后先释放，再重新按下，避免自动继续旧会话。
App 若未订阅 `0xFFA1`，即使 READY，打开 VOICE 仍可能等待；不能只凭 `Serial PTT pressed` 判断开始成功。

## 预期结果与局限

正常串口输出包括 `Serial PTT pressed`、`PTT started`、`Serial PTT released` 和关闭结果。
VOICE 数据经 GATT 通知发送；App/平台可能收到测试音，但不能据此期待人声识别或正常对话。
收到 TTS 时本例不会出声，`tts_bytes` 只能在调试器查看累计值，结束时会重置。

队列满或超时时，本例允许丢弃当前实时 PCM 块，并把本轮标为不完整；不保证录音连续无丢块。
`closeStream()` 成功只表示本地接受关闭，后台可能继续排空，程序会等待再开下一段。
VOICE 没有线上的独立 END 标记，`complete=false` 是本地失败记录，不等于能撤回已提交数据。
永久结束错误会禁止继续复用该会话，需重新连接；断连会使旧流句柄失效。

本例不含真实音频硬件验证，流状态完成也不是对端 ACK。
详细约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)，其他练习见[示例索引](../README.zh-CN.md)。
