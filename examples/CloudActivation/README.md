# CloudActivation：云端激活与绑定

演示 Wi-Fi 配网、平台鉴权、输出激活码以及本地解绑请求。适合已有兼容平台部署和产品配置的开发者；示例没有内置可用的服务器或账号。

## 准备与配置

- 带 Wi-Fi 的 ESP32、官方 ESP32 Arduino Core、本库，以及可连接的网络。
- 平台提供的 HTTPS 根地址、正整数 `product_id`、芯片型号和可完成绑定的账号。
- 按实际 Flash 选择足够大的应用分区；ESP32-S3 已验证的选项是 `min_spiffs`。
- 建议先用 [BasicWiFi](../BasicWiFi/README.md) 或 [WiFiProvisioning](../WiFiProvisioning/README.md) 跑通网络。

打开 [CloudActivation.ino](CloudActivation.ino)，修改：

| 配置 | 修改内容 |
| --- | --- |
| `platform.base_url` | 自己的平台 HTTPS 根地址，不带末尾 `/`；`https://api.example.com` 是占位符 |
| `DEOTALAND_CLOUD_PRODUCT_ID` | 把默认 `0` 改为平台提供的正整数产品 ID |
| `platform.chip_type` | 平台认可且与实际设备相符的芯片型号，默认 `esp32-s3` |
| `config.model` / `firmware_rev` | 自己的设备型号和应用固件版本，不必与本库版本一致 |

产品 ID 未填写时，程序会提示修改，并且不会启动网络与平台流程。

## 使用步骤

1. 填好上述配置，按实际板型和端口编译上传。
2. 打开 **115200** 串口；如错过初始提示，可复位开发板。
3. 本例未设置 `config.wifi`：设备使用已保存网络，或按串口提示连接热点并在 [配网页](http://192.168.4.1/) 保存网络。
4. 等待平台状态进入 `PAIRING`，串口会输出 **`Activation code`** 和有效期秒数。
5. 在对应平台的绑定流程中使用激活码，在有效期内完成绑定。
6. 根据后续状态、错误 `Detail` 和设备 UUID 确认平台结果。

## 串口操作

| 输入 | 作用 |
| --- | --- |
| 小写 `f` | 请求清除本地平台配对凭据；保留设备 UUID 和 Wi-Fi 凭据 |

`Forget request: ESP_OK` 表示本地接受请求；平台控制台已有的绑定记录可能仍需手动解除。复用设备测试时，应确认平台侧的设备和产品配置匹配。

## 预期结果与范围

云端在线对应 `CONNECTED`，不是 Agent 数据通道 `READY`。当前 Wi-Fi 后端仍不提供控制或媒体数据通道，不能等待 `isReady()` 为真后才查看激活码，也不能用本例发送图片或音频。

联网不成功时先排查 SSID、密码和信号；平台报错时按实际 `title`、`hint`、`detail` 核对平台地址、产品配置和鉴权。安装与排错见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，全部示例见[索引](../README.zh-CN.md)。
