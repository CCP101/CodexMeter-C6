# 安全说明

## 凭据管理

- 不要把真实 Wi-Fi 密码、MQTT 用户名、密码、令牌或私钥提交到仓库。
- 固件本地配置保存在 `src/secrets.h`；该文件已被 `.gitignore` 排除。公开内容只包含 `src/secrets.example.h` 占位模板。
- MQTT 发布脚本优先从 `CODEX_C6_MQTT_*` 环境变量读取连接信息。避免在共享终端记录或命令历史中直接传入密码。
- `tools/write_snapshot.py` 只生成显示所需的紧凑额度快照，不应输出或传输 OAuth 令牌、Cookie 或账号凭据。

## 网络边界

当前固件实现 MQTT 3.1.1 明文 TCP，默认端口为 1883，不支持 TLS 证书校验。仅应在可信且隔离的局域网中使用；不要把 MQTT broker 暴露到公网。需要跨不可信网络时，应先增加 TLS、证书校验和安全配网。

## 报告漏洞

请通过 GitHub 仓库的 Security Advisory 私下报告可能导致凭据泄露、未授权网络访问或设备异常的问题。请勿在公开 Issue 中粘贴真实凭据、网络地址、日志原文或账号数据。
