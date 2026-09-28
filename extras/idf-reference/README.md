# IDF board reference sources

这里同步保留新版工程的板卡实现、共享驱动、`Board` 抽象和显示组件，便于把真实硬件接入 Arduino 草图；包含 ES8311 语音/ASR、GC2145 相机、RoRoLee、TemMonitor 和 ESP32-P4 参考代码。

`extras/` 不会被 Arduino 构建器自动编译。这些文件仍使用 ESP-IDF 的板卡组织方式，其中 RoRoLee-S3 还依赖 Component Manager 的 `esp_codec_dev`，Waveshare ESP32-P4 依赖 MIPI-DSI/CO5300 组件。硬件接入约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)；将所需驱动拆成独立 Arduino 硬件库或复制到具体草图中，不要把整个目录加入通用库的 `src/`。
