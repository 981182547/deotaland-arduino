# WiFiProvisioning：通过设备热点配网

演示不把 Wi-Fi 密码写入草图的配网流程。设备优先使用已保存凭据；需要配网时开启热点和网页，提交网络配置后尝试连接路由器，连接成功后保存凭据。

## 准备

- 带 Wi-Fi 的 ESP32、官方 ESP32 Arduino Core、本库。
- 手机或电脑，用于连接设备热点并打开浏览器。
- 路由器的 SSID 和密码；不需要 BLE App 或云平台账号。

## 使用步骤

1. 打开 [WiFiProvisioning.ino](WiFiProvisioning.ino)，按实际板型和端口编译上传。
2. 打开 **115200** 串口，查看 `begin` 结果及后续状态提示。
3. 无可用已保存凭据、门户开启时，连接提示中的开放热点 **`Deotaland-WiFi-XXXX`**。
4. 若手机没有自动打开配网页，在浏览器访问 [http://192.168.4.1/](http://192.168.4.1/)。
5. 填写并提交网络配置。保持与设备热点连接完成操作；热点没有互联网属于正常现象。
6. 等待设备连接路由器，串口应打印 `Wi-Fi IP: ...`。
7. 重启设备，观察已保存网络的连接流程。

如果设备已有可用凭据，会直接尝试连接，而不是每次都出现配网热点。以串口中的 `hint` 和 `wifi_portal_active` 为准。

## 配置与状态

本例保持 `config.wifi == nullptr`，交给库读取已保存凭据或启动门户。要在草图中显式填写网络，可参考 [BasicWiFi](../BasicWiFi/README.md)；无需再调用 `WiFi.begin()`。

`onStatus` 回调复制状态快照，`loop()` 持续调用 `poll()` 并限频打印。状态变化时最快约每 500 ms 输出，其他时候约每 10 秒输出一次。**本例没有串口命令**。

- `wifi_connected` 和 `wifi_ip` 用于判断路由器连接。
- `wifi_portal_active` 表示网页配网服务是否开启。
- 有 IP 不保证互联网可用；没有平台配置时不执行云端激活。
- 当前 Wi-Fi 后端不提供 Agent 提示词或音视频通道，`isReady()` 不会变为 `true`。

下一步可运行 [CloudActivation](../CloudActivation/README.md)。安装与排错见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，全部示例见[索引](../README.zh-CN.md)。
