# BufferedPlayback：PCM 缓冲与播放背压

[源码](BufferedPlayback.ino)演示 `configurePlayback()` 的部分接收、真实队列水位和非阻塞播放步进。
默认板级驱动不消费 PCM，**运行本例不会出声**；它可以用来观察缓冲填满后的背压。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- 支持 Agent Link TTS 下行的 App；需建立 CoC `PSM 0x0081`，并发送对应的语音回复开始/结束命令。
- 基础练习无需扬声器。要真正播放，需要自己接入音频编解码器/I2S/DMA 驱动和实际接线。
- 先跑通 BasicBLE；通道配置见 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。

## 配置与硬件替换点

库向播放回调交付 16 kHz、单声道、16 位 PCM，每毫秒为 32 字节。
本例应用环形队列为 `kQueueBytes = 8192`，容量 256 ms；它与库内部 PCM 暂存区分别占用内存。
每轮最多向板级驱动提交 `kHardwareChunkBytes = 512` 字节。

要接入扬声器，必须同时实现下面三个函数，并在 `setup()` 初始化驱动与固定缓冲：

| 替换点 | 返回或执行的内容 |
| --- | --- |
| `boardWritePcmNonBlocking()` | 用零等待提交 PCM，返回实际接受的偶数字节数；0 表示稍后重试 |
| `boardQueuedPcmBytes()` | 返回驱动已接受但尚未播放的 PCM 字节，包括正在播放的 DMA 缓冲 |
| `boardCancelPcmNonBlocking()` | 快速取消/丢弃旧会话的驱动队列 |

写入函数返回前须把接受的内容复制到驱动自己的缓冲，不能让 DMA 继续借用随后会被复用的环形队列。
回调不能等待播放完成、临时分配缓冲或调用阻塞式 I2S 写入。
`queuedPcm()` 将应用环形队列和驱动/DMA 队列相加，每个字节只计算一次。
只有实现真实硬件后，才按实际容量调整 `setPlaybackBufferMs()`；不能用虚报容量消除背压。

## 上传与操作

1. 打开 [BufferedPlayback.ino](BufferedPlayback.ino)，按[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)配置并上传。
2. 打开 115200 串口监视器，读取启动和 CoC 能力诊断。
3. 在 App 连接 `Buffered-Playback` 并订阅事件 `0xFFC4`，再打开音频 CoC `0x0081`。
4. 用 App 的 TTS/语音回复功能发送一段音频；本例没有串口触发 TTS 的命令。
5. 串口发送 `?` 查看控制状态及待消费 PCM 字节；程序也约每 10 秒打印诊断。

`?` 是唯一串口命令，可附加换行。诊断限频，不逐个 PCM 包打印。
READY 只表示控制通道可用，不能证明 App 已建立 CoC 或已经发送 TTS。

## 预期结果与局限

默认运行时先看到“未接扬声器驱动，默认不会出声”的诊断。
收到 PCM 后，`used` 增加，较长音频会填满 8192 字节环形队列；板级写入返回 0，所以队列不会自行排空。
库保留未被接受的 PCM 后缀，并根据真实水位产生背压，而不是假定音频已经播放。

`shortAccepts`、`invalidDriverReturns`、`receivedEnds` 可在调试器中查看。
`onAudioEnd()` 表示完整 PCM 已交给应用，环形队列和 DMA 仍可能有音频；物理播放完成需等待总水位为 0。
默认长音频若被满队列挡住，结束回调也可能暂不触发；不要在 END 回调中清空正常待播放数据。
离开 READY 时会清空应用环形队列，并调用取消驱动函数，避免把旧会话音频带到下一连接。

本例不含麦克风、扬声器驱动或真实播放验证；`poll(4, 1000)` 的预算也不能抢占阻塞的用户回调。
播放接口细节见 [API 参考](../../docs/REFERENCE.zh-CN.md)，其他练习见[示例索引](../README.zh-CN.md)。
