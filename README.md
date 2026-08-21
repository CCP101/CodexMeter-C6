# CodexMeter C6：Codex 周额度 AMOLED 监视器

CodexMeter C6 是为 Waveshare ESP32-C6-Touch-AMOLED-1.43（466×466、CO5300）制作的桌面额度监视器。Windows 主机把 Codex 七天额度压缩为最小化 JSON 快照并发布到 MQTT；设备通过 2.4 GHz Wi-Fi 订阅 retained 消息，在圆形 AMOLED 上显示剩余额度、重置时间、套餐和模型信息。

本仓库不包含 Wi-Fi 密码、MQTT 凭据、OAuth 令牌、Cookie 或用户目录路径。所有本地连接信息都通过未跟踪的配置文件或环境变量提供。

## 功能

- 显示七天额度的剩余百分比、已用比例和重置倒计时。
- 使用 RGB565 覆盖率混色位图绘制抗锯齿圆环，避开 CO5300 上 `fillArc()` 的端点伪影。
- MQTT 断线自动重连，并定期重新订阅 retained 主题。
- 保留 USB CDC 快照入口，便于无 MQTT 环境下诊断显示与解析逻辑。
- 只接收显示所需字段，设备端不保存 Codex 登录态或账号凭据。

## 硬件与软件

- Waveshare ESP32-C6-Touch-AMOLED-1.43 Rev1.3
- Arduino CLI
- Arduino-ESP32 3.3.11
- GFX Library for Arduino 1.6.4
- Adafruit XCA9554 1.0.0（依赖 Adafruit BusIO）
- Python 3.10+ 与 pyserial 3.5（仅 USB CDC 工具需要）
- 可访问的 MQTT 3.1.1 broker，以及能够输出 Codex 额度状态的本地主机导出脚本

依赖许可证和上游链接见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 数据链路

```text
Codex 本地状态
  -> 主机额度导出脚本
  -> tools/publish-codex-usage-c6-mqtt.ps1
  -> MQTT retained 主题
  -> ESP32-C6
  -> CO5300 AMOLED
```

设备只接受版本为 `v = 1` 的紧凑快照，并以 `windowMins = 10080` 识别七天窗口。不要把包含历史桶和大量字段的完整主机状态直接发送给设备。

## 快速开始

### 1. 安装构建依赖

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "GFX Library for Arduino@1.6.4" "Adafruit XCA9554@1.0.0"
python -m pip install -r requirements.txt
```

### 2. 创建本地凭据文件

```powershell
Copy-Item .\src\secrets.example.h .\src\secrets.h
```

编辑 `src/secrets.h`，填写自己的 2.4 GHz Wi-Fi 和 MQTT 参数。该文件不会被 Git 跟踪。提交前仍应运行 `git status`，不要因为有 `.gitignore` 就跳过检查，老师——凭据预算可没有透支额度。

### 3. 编译与刷写

```powershell
$fqbn = 'esp32:esp32:esp32c6:FlashSize=16M'
arduino-cli compile --fqbn $fqbn --build-path .\.arduino-build .
arduino-cli upload --fqbn $fqbn --port COM3 --input-dir .\.arduino-build
```

`COM3` 只是示例；请以设备管理器或 `arduino-cli board list` 当前识别到的端口为准。

## 发布 MQTT retained 快照

`tools/publish-codex-usage-c6-mqtt.ps1` 会调用一个本机额度导出脚本的 `-DryRun` 模式，并把七天额度压缩后发布到设备主题。这个外部导出脚本不属于本仓库，必须通过环境变量或 `-ReferenceScript` 指定。

推荐把连接信息只放在当前 PowerShell 会话的环境变量中：

```powershell
$env:CODEX_USAGE_EXPORTER_SCRIPT = 'C:\path\to\publish-codex-usage-mqtt.ps1'
$env:CODEX_C6_MQTT_HOST = 'mqtt.lan'
$env:CODEX_C6_MQTT_USERNAME = 'device-publisher'
$env:CODEX_C6_MQTT_PASSWORD = 'replace-with-local-secret'
$env:CODEX_C6_MQTT_TOPIC = 'codex/usage/c6/state'

pwsh -NoProfile -File .\tools\publish-codex-usage-c6-mqtt.ps1 -DryRun
pwsh -NoProfile -File .\tools\publish-codex-usage-c6-mqtt.ps1
```

`-DryRun` 只打印紧凑 JSON，不连接 MQTT。真实发布默认使用 retained 消息；可用 `-NoRetain` 临时关闭。

## USB CDC 诊断入口

`tools/write_snapshot.py` 可以直接从 Codex app-server、外部导出脚本或手工参数生成快照。默认刷新间隔为两小时，持续模式的最小间隔为五分钟。

```powershell
# 只生成快照，不访问串口
python .\tools\write_snapshot.py --source codex --dry-run

# 手工测试设备解析与显示
python .\tools\write_snapshot.py --source manual --remaining 73 --window-mins 10080 --port COM3

# 使用外部导出脚本持续刷新
$env:CODEX_USAGE_EXPORTER_SCRIPT = 'C:\path\to\publish-codex-usage-mqtt.ps1'
python .\tools\write_snapshot.py --source exporter --watch --interval 7200 --port COM3
```

## 本地验证

```powershell
python -m unittest discover -s tools -p 'test_*.py'
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:FlashSize=16M' --build-path .\.arduino-build .
```

构建前必须存在本地 `src/secrets.h`。发行检查还应确认 `git status` 中没有该文件、构建目录、日志或缓存。

## 已知限制

- MQTT 当前使用明文 TCP/1883，不支持 TLS；只适合可信局域网。详见 [SECURITY.md](SECURITY.md)。
- Wi-Fi/MQTT 凭据仍会编译进设备固件，只是不会进入 Git 仓库。后续可增加安全配网与设备端凭据存储。
- 当前未启用触摸功能。
- 快照保存在 RAM 中；设备重启后依赖 MQTT retained 消息恢复。
- MQTT 客户端是面向本项目最小需求的实现，不是通用 MQTT 库。

## 许可证

项目自身源码采用 [MIT License](LICENSE)。第三方依赖保留其各自许可证与版权。
