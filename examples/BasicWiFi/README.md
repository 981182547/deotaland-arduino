# BasicWiFi：填写凭据连接路由器

通过预设 SSID 和密码连接路由器，并从库的状态快照读取 IP 地址。适合先验证 Wi-Fi 网络，不需要 BLE 配对或云平台配置。

## 准备与配置

- 带 Wi-Fi 的 ESP32、官方 ESP32 Arduino Core、本库和可连接的路由器。
- 在 [BasicWiFi.ino](BasicWiFi.ino) 顶部修改 `kSsid` 和 `kPassword`。
- 开放网络的密码填 `""`；保留 `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD` 时，程序只提示修改，不会启动连接。
- 按实际板型、Flash 大小和端口设置 Arduino IDE；安装与串口说明见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)。

## 使用步骤

1. 填好网络凭据，编译并上传草图。
2. 打开 **115200** 串口，等待连接状态和 IP 输出。
3. 看到 `Wi-Fi 已连接，IP: ...` 即表示本例目标完成。
4. 若出现配网热点提示，按提示连接 **`Deotaland-WiFi-XXXX`**，在浏览器打开 [配网页](http://192.168.4.1/) 填写网络。

| 串口输入 | 作用 |
| --- | --- |
| `?` | 请求打印当前状态，输出间隔至少约 500 ms |

默认约每 5 秒打印一次状态。`begin` 成功表示后台服务已启动，仍需等待获得 IP。

## 如何判断结果

| 字段 / 阶段 | 含义 |
| --- | --- |
| `wifi_connected=true` / `wifi_ip` | 已获得路由器 IP |
| `wifi_portal_active=true` | 配网门户开启，按 `hint` 指示操作 |
| `NETWORK_READY` | 无云平台配置，网络已连接且门户已关闭 |
| `BLOCKED` | 查看 `title`、`hint` 和 `detail` 的错误信息 |

`wifi_connected` 不保证互联网或云平台可达。当前 Wi-Fi 后端没有 Agent 控制和媒体数据通道，`isReady()` 不会变成 `true`。

## 换网与后续

草图中的显式 SSID 每次启动优先于已保存凭据。通过门户换网后，也要更新草图中的配置；希望一直使用设备保存的网络时，改用 [WiFiProvisioning](../WiFiProvisioning/README.md)。Wi-Fi 生命周期由本库管理，不要再在同一草图调用 `WiFi.begin()`。

需要激活和绑定云平台时继续 [CloudActivation](../CloudActivation/README.md)，其他示例见[索引](../README.zh-CN.md)。
