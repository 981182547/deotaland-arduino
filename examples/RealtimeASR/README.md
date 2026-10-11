# RealtimeASR：App 发起 AUDIO 采集

[源码](RealtimeASR.ino)演示 App 开始/停止采集、AUDIO 流打开、定时写入和结束重试。
**默认产生合成测试 PCM，不采集麦克风，也不能用于识别人声。** ASR 业务仍依赖 App 和平台。

## 前置条件

- 有 BLE 控制器的 ESP32、兼容的 ESP32 Arduino Core 和本库。
- 支持 Agent Link 采集命令和 AUDIO CoC 的 App。
- App 订阅事件 `0xFFC4` 进入 READY，并建立音频 CoC `PSM 0x0081`。
- 无需麦克风接线即可验证会话；真正录音需自己的麦克风/编解码器驱动。
- 先按[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)跑通 BasicBLE，再核对 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。

## 配置与硬件替换点

设备名称为 `Deotaland-ASR`，声明 MIC 和 RECORDING 能力，注册 `onListen()` 接收采集请求。
将 `makeTestPcm()` 替换为已采集的 16 kHz、单声道、有符号 16 位 PCM。
本例每约 20 ms 生成 320 个采样，即 640 字节；真实驱动应使用自己的固定缓冲和及时返回的读取步骤。
初始化实际 I2S/codec 和引脚应在应用中完成，本例不指定通用麦克风 GPIO。

`onListen()` 和状态回调由 `Deotaland.poll()` 在 loop 线程派发。
回调只记录任务状态，不应阻塞等麦克风或从回调执行 begin/end。
启动后的 AUDIO 能力查询只检查后端支持，不分配流缓存；状态 UNAVAILABLE 可能只是 App 尚未连接。

## 上传与操作

1. 打开 [RealtimeASR.ino](RealtimeASR.ino)，按真实板卡选择设置并上传。
2. 打开 115200 串口，确认启动成功和“后端支持 AUDIO”的诊断。
3. 在 App 连接 `Deotaland-ASR`，订阅 `0xFFC4` 并打开 CoC `0x0081`。
4. 在 App 发起录音/实时采集；默认会发送测试音。串口不能直接开始采集。
5. 用 App 停止采集，或等待命令指定的最长时长，查看打开/关闭流的结果。
6. 串口输入 `?` 重看诊断；程序也约每 10 秒打印一次，输入可附加换行。

## App 命令与状态

| 命令 | 载荷与行为 |
| --- | --- |
| `StartCapture (0x3C)` | 3 字节：mode + 最大时长 LE16；调用 `onListen(true, max_ms)` |
| `StopCapture (0x3D)` | 空载荷；请求结束当前采集 |

当前核心保留 mode 字节，但本例不根据它选择硬件或编码模式。
最大时长为 0 时没有自动时限，需要 App 停止；非零时从收到采集请求开始计时，包括等待通道的时间。
READY 只表示控制就绪，AUDIO 打开可能仍等待对端 CoC；暂时错误至少间隔 100 ms 重试打开。

## 预期结果与局限

收到可执行的采集请求后，串口会输出 `openStream(AUDIO): ESP_OK`。
停止时正常输出 `closeStream(AUDIO): ESP_OK`；状态和能力信息可通过 `?` 查看。
未发起采集时不会自动上传音频，串口 `p`/`r` 也没有定义。

实时写入若遇到 NO_MEM/TIMEOUT，本例跳过当前 20 ms 测试块，并把会话标为不完整。
这保持循环响应，但不保证连续无丢块录音；需要完整录音时应另做有界缓存和数据保留。
永久写入错误会停止任务；永久结束错误会阻止后续采集，需重新连接以建立新会话。
断连会丢弃旧采集请求和句柄，重连后需由 App 重新发起采集。

流的成功表示本地接受，不是对端 ACK，也不保证平台产生识别结果。
本例没有人声采集、语音识别模型、识别文字显示或扬声器播放实现。
详细约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)，其他语音练习见[示例索引](../README.zh-CN.md)。
