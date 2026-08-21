# 第三方软件声明

本仓库只保存项目自身源码，不复制下列第三方库源码。构建时需要另行安装这些依赖，并遵守各自的许可证。

| 依赖 | 验证版本 | 用途 | 许可证 | 上游项目 |
| --- | --- | --- | --- | --- |
| Arduino-ESP32 | 3.3.11 | ESP32-C6 Arduino 核心、Wi-Fi 与 USB CDC | LGPL-2.1，部分组件可能采用其他兼容许可证 | <https://github.com/espressif/arduino-esp32> |
| GFX Library for Arduino | 1.6.4 | CO5300 QSPI AMOLED 驱动与绘图 | BSD-2-Clause | <https://github.com/moononournation/Arduino_GFX> |
| Adafruit XCA9554 | 1.0.0 | XCA9554 GPIO 扩展器 | BSD | <https://github.com/adafruit/Adafruit_XCA9554> |
| Adafruit BusIO | 1.17.4 | Adafruit XCA9554 的 I²C 依赖 | MIT | <https://github.com/adafruit/Adafruit_BusIO> |
| pyserial | 3.5 | 可选的 USB CDC 串口诊断工具 | BSD-3-Clause | <https://github.com/pyserial/pyserial> |

各依赖的完整许可证文本以其上游仓库和安装包为准。若分发包含这些依赖的固件二进制或安装包，请同时核对对应版本的再分发义务与版权声明。本文件不是法律意见。

## 参考项目

硬件初始化与引脚配置参考 Waveshare 示例和以下项目；本仓库未直接打包其源码：

- Waveshare ESP32-C6-Touch-AMOLED-1.43 硬件资料：<https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.43>
- Waveshare Codex Meter 参考项目：<https://github.com/waveshareteam/codex-meter>
