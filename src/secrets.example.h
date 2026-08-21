#pragma once

// 复制为 secrets.h 后填写本地配置。secrets.h 已被 .gitignore 排除。
constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";

// 当前固件使用未加密的 MQTT 3.1.1/TCP；仅建议用于可信局域网。
constexpr char kMqttHost[] = "192.0.2.10";
constexpr uint16_t kMqttPort = 1883;
constexpr char kMqttUsername[] = "YOUR_MQTT_USERNAME";
constexpr char kMqttPassword[] = "YOUR_MQTT_PASSWORD";
constexpr char kMqttTopic[] = "codex/usage/c6/state";
