# SerialPrompt：通过串口发送提示词

在 BLE 事件通道就绪后，将预设文本发送给 Agent。示例包含单条待发队列、拥塞重试，以及超时和连接变化时的取消处理。

## 准备

- 带 BLE 的 ESP32、官方 ESP32 Arduino Core、本库和兼容 Agent Link 的 App。
- App、账号和平台需提供实际 Agent 业务，先用 [BasicBLE](../BasicBLE/README.md) 验证连接。
- 无需实体按键或其他外设。

## 使用步骤

1. 打开 [SerialPrompt.ino](SerialPrompt.ino)，按实际板型和端口上传。
2. 打开 **115200** 串口，在 App 中连接 **`Deotaland-Prompt`** 并订阅事件特征 `0xFFC4`。
3. 串口发送 `?`，确认状态是 `READY`。
4. 串口发送 `p`，向 Agent 提交预设的“你好”。
5. 查看串口提交结果，并到 App 中确认实际业务响应。

| 串口输入 | 作用 |
| --- | --- |
| `p` / `P` | 提交一条预设提示词 |
| `?` | 查询连接状态和 `maxPromptBytes()` |

可以附带换行；程序会忽略换行及其他字符。重复输入时只允许一条待发提示词。

## 修改提示词

修改 `kPrompt`，然后重新编译上传。文本容量按 **UTF-8 字节数**计算，默认“你好”为 6 字节；允许容量通过 `maxPromptBytes()` 查询，并受当前 BLE MTU 影响。

一个提示词必须放入一帧。超长文本应缩短或先由 App 协商更大的 MTU，不能假定多次 `pushPrompt()` 会自动拼成一句话。

## 重试与结果

- 可重试错误每 100 ms 再尝试一次，期间继续 `poll()`。
- 待发超过 5 秒或连接离开 `READY` 时取消，旧提示词不会留到新会话。
- 示例没有手动取消按键。超时后可再次输入 `p`。
- “已进入本地发送队列”只表示本地接受；实际 Agent 回复取决于 App 和平台业务。

排错见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，其他示例见[索引](../README.zh-CN.md)。
