#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <math.h>

#include "Arduino_GFX_Library.h"
#include <Adafruit_XCA9554.h>
#include "HWCDC.h"

#include "pin_config.h"

namespace {

constexpr size_t kSerialLineCapacity = 2304;
constexpr size_t kMqttPayloadCapacity = 1024;
constexpr uint16_t kBackground = 0x0000;
constexpr uint16_t kCard = 0x18C3;
constexpr uint16_t kPanel = 0x39C7;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kMuted = 0xA514;
constexpr uint16_t kTertiary = 0x630C;
constexpr uint16_t kBlue = 0x0C1F;
constexpr uint16_t kGreen = 0x368A;
constexpr uint16_t kAmber = 0xFCE1;
constexpr uint16_t kRed = 0xFA27;
constexpr uint32_t kDefaultPollSeconds = 2UL * 60UL * 60UL;
constexpr uint32_t kMqttRefreshSeconds = 30UL * 60UL;
constexpr uint32_t kMqttRefreshIntervalMs = kMqttRefreshSeconds * 1000UL;
constexpr uint32_t kWifiRetryIntervalMs = 10UL * 1000UL;
constexpr uint32_t kMqttRetryIntervalMs = 5UL * 1000UL;
constexpr uint32_t kMqttPacketTimeoutMs = 2000UL;
constexpr uint16_t kMqttKeepAliveSeconds = 60;
constexpr uint32_t kFreshnessGraceSeconds = 5UL * 60UL;
constexpr uint32_t kMinimumRenderIntervalMs = 5UL * 60UL * 1000UL;
constexpr int16_t kRingCenterX = LCD_WIDTH / 2;
constexpr int16_t kRingCenterY = 208;
constexpr int16_t kRingOuterRadius = 132;
constexpr int16_t kRingInnerRadius = 112;
constexpr int16_t kRingMiddleRadius = (kRingOuterRadius + kRingInnerRadius) / 2;
constexpr int16_t kRingStrokeRadius = (kRingOuterRadius - kRingInnerRadius) / 2;
constexpr int16_t kRingBitmapMargin = 1;
constexpr int16_t kRingBitmapSize = 2 * (kRingOuterRadius + kRingBitmapMargin) + 1;

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h; copy src/secrets.example.h and fill in local values."
#endif

#if __has_include("ota.local.h")
#include "ota.local.h"
#else
constexpr char kOtaPasswordHash[] = "";
#endif
constexpr char kOtaHostname[] = "codexmeter-c6";
constexpr uint16_t kOtaPort = 3232;

enum SnapshotLinkState : uint8_t {
  kLinkLive = 0,
  kLinkCached = 1,
  kLinkOffline = 2,
};

struct UsageSnapshot {
  bool valid = false;
  int remaining = 0;
  int used = 100;
  int windowMins = 0;
  long resetsIn = -1;
  uint32_t receivedAt = 0;
  long capturedAt = -1;
  long nextPollIn = -1;
  SnapshotLinkState linkState = kLinkLive;
  char planLabel[32] = "UNKNOWN";
  char modelLabel[32] = "MODEL UNKNOWN";
  char source[20] = "MQTT EXPORTER";
};

Adafruit_XCA9554 expander;
HWCDC USBSerial;
WiFiClient mqttClient;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RST, 0, LCD_WIDTH, LCD_HEIGHT,
    LCD_COLUMN_OFFSET, 0, 0, 0);

UsageSnapshot currentSnapshot;
bool displayReady = false;
bool expanderReady = false;
bool lcdEnableReady = false;
bool inputOverflow = false;
size_t inputLength = 0;
char inputLine[kSerialLineCapacity];
char mqttPayload[kMqttPayloadCapacity];
uint16_t ringBitmap[kRingBitmapSize * kRingBitmapSize];
uint32_t lastRenderAt = 0;
uint32_t nextWifiAttemptAt = 0;
uint32_t nextMqttAttemptAt = 0;
uint32_t nextMqttRefreshAt = 0;
uint32_t lastMqttActivityAt = 0;
uint16_t mqttPacketId = 0;
bool wifiStarted = false;
bool wifiReported = false;
bool mqttReported = false;
bool otaStarted = false;
bool otaInProgress = false;
int lastOtaProgress = -1;

void reportDiagnostics();
void renderSnapshot();

void drawTextAt(const char *text, int16_t x, int16_t y, uint8_t size,
                uint16_t color)
{
  gfx->setTextSize(size);
  gfx->setTextColor(color, kBackground);
  gfx->setCursor(x, y);
  gfx->print(text);
}

const char *jsonValueStart(const char *json, const char *key)
{
  const char *keyStart = strstr(json, key);
  if (!keyStart) {
    return nullptr;
  }

  const char *value = keyStart + strlen(key);
  while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
    ++value;
  }
  if (*value != ':') {
    return nullptr;
  }
  ++value;
  while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
    ++value;
  }
  return value;
}

bool jsonNumber(const char *json, const char *key, long *result)
{
  const char *value = jsonValueStart(json, key);
  if (!value) {
    return false;
  }

  char *end = nullptr;
  const long parsed = strtol(value, &end, 10);
  if (end == value) {
    return false;
  }
  *result = parsed;
  return true;
}

bool jsonString(const char *json, const char *key, char *target, size_t targetSize)
{
  const char *value = jsonValueStart(json, key);
  if (!value || *value != '"' || targetSize == 0) {
    return false;
  }

  ++value;
  size_t length = 0;
  while (*value && *value != '"') {
    if (*value == '\\' || length + 1 >= targetSize) {
      return false;
    }
    target[length++] = *value++;
  }
  if (*value != '"') {
    return false;
  }
  target[length] = '\0';
  return true;
}

int clampPercent(long value)
{
  if (value < 0) return 0;
  if (value > 100) return 100;
  return static_cast<int>(value);
}

bool parseSnapshot(const char *json, UsageSnapshot *snapshot)
{
  if (!json || !snapshot) {
    return false;
  }

  long version = 0;
  if (!jsonNumber(json, "\"v\"", &version) || version != 1) {
    return false;
  }

  const char *preferred = strstr(json, "\"preferred\"");
  const char *primary = preferred ? strstr(preferred, "\"primary\"") : nullptr;
  if (!primary) {
    return false;
  }

  long used = 0;
  long remaining = 0;
  const bool hasUsed = jsonNumber(primary, "\"used\"", &used);
  const bool hasRemaining = jsonNumber(primary, "\"remaining\"", &remaining);
  if (!hasUsed && !hasRemaining) {
    return false;
  }

  UsageSnapshot parsed;
  parsed.valid = true;
  parsed.used = hasUsed ? clampPercent(used) : 100 - clampPercent(remaining);
  parsed.remaining = hasRemaining ? clampPercent(remaining) : 100 - parsed.used;

  long windowMins = 0;
  long resetsIn = -1;
  if (jsonNumber(primary, "\"windowMins\"", &windowMins)) {
    parsed.windowMins = max(0L, windowMins);
  }
  if (jsonNumber(primary, "\"resetsIn\"", &resetsIn)) {
    parsed.resetsIn = resetsIn;
  }

  char status[16] = "ok";
  if (jsonString(json, "\"status\"", status, sizeof(status))) {
    if (strcmp(status, "stale") == 0 || strcmp(status, "refreshing") == 0) {
      parsed.linkState = kLinkCached;
    } else if (strcmp(status, "offline") == 0 || strcmp(status, "error") == 0) {
      parsed.linkState = kLinkOffline;
    }
  }

  long nextPollIn = -1;
  if (jsonNumber(json, "\"nextPollIn\"", &nextPollIn)) {
    parsed.nextPollIn = max(0L, nextPollIn);
  }

  long capturedAt = -1;
  if (jsonNumber(json, "\"capturedAt\"", &capturedAt)) {
    parsed.capturedAt = capturedAt;
  }

  char source[sizeof(parsed.source)] = "USB CDC";
  if (jsonString(json, "\"source\"", source, sizeof(source))) {
    snprintf(parsed.source, sizeof(parsed.source), "%s", source);
  }

  char planLabel[sizeof(parsed.planLabel)] = "UNKNOWN";
  if ((jsonString(json, "\"planLabel\"", planLabel, sizeof(planLabel)) && planLabel[0]) ||
      (jsonString(json, "\"plan\"", planLabel, sizeof(planLabel)) && planLabel[0])) {
    snprintf(parsed.planLabel, sizeof(parsed.planLabel), "%s", planLabel);
  }
  if (strcasecmp(parsed.planLabel, "codex") == 0) {
    snprintf(parsed.planLabel, sizeof(parsed.planLabel), "UNKNOWN");
  }

  char modelLabel[sizeof(parsed.modelLabel)] = "MODEL UNKNOWN";
  if (jsonString(json, "\"modelLabel\"", modelLabel, sizeof(modelLabel)) ||
      jsonString(json, "\"model\"", modelLabel, sizeof(modelLabel))) {
    snprintf(parsed.modelLabel, sizeof(parsed.modelLabel), "%s", modelLabel);
  }

  parsed.receivedAt = millis();
  *snapshot = parsed;
  return true;
}

bool mqttAppendUtf8(uint8_t *target, size_t capacity, size_t *length,
                    const char *value)
{
  if (!target || !length || !value) return false;

  const size_t valueLength = strlen(value);
  if (valueLength > 65535 || *length + 2 + valueLength > capacity) {
    return false;
  }

  target[(*length)++] = static_cast<uint8_t>((valueLength >> 8) & 0xFF);
  target[(*length)++] = static_cast<uint8_t>(valueLength & 0xFF);
  memcpy(target + *length, value, valueLength);
  *length += valueLength;
  return true;
}

bool mqttSendPacket(uint8_t packetType, const uint8_t *payload,
                    size_t payloadLength)
{
  if (!mqttClient.connected() || payloadLength > 268435455UL) {
    return false;
  }

  if (mqttClient.write(&packetType, 1) != 1) {
    return false;
  }

  size_t remaining = payloadLength;
  do {
    uint8_t digit = static_cast<uint8_t>(remaining % 128);
    remaining /= 128;
    if (remaining > 0) digit |= 0x80;
    if (mqttClient.write(&digit, 1) != 1) {
      return false;
    }
  } while (remaining > 0);

  if (payloadLength > 0 &&
      mqttClient.write(payload, payloadLength) != payloadLength) {
    return false;
  }

  lastMqttActivityAt = millis();
  return true;
}

bool mqttReadByte(uint8_t *value, uint32_t timeoutMs)
{
  if (!value) return false;

  const uint32_t startedAt = millis();
  while (mqttClient.available() <= 0) {
    if (!mqttClient.connected() || millis() - startedAt >= timeoutMs) {
      return false;
    }
    delay(1);
  }

  const int readValue = mqttClient.read();
  if (readValue < 0) return false;
  *value = static_cast<uint8_t>(readValue);
  lastMqttActivityAt = millis();
  return true;
}

bool mqttReadBytes(uint8_t *target, size_t length, uint32_t timeoutMs)
{
  for (size_t index = 0; index < length; ++index) {
    if (!mqttReadByte(target + index, timeoutMs)) return false;
  }
  return true;
}

bool mqttReadRemainingLength(uint32_t *length)
{
  if (!length) return false;

  uint32_t multiplier = 1;
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    uint8_t digit = 0;
    if (!mqttReadByte(&digit, kMqttPacketTimeoutMs)) return false;
    value += (digit & 0x7F) * multiplier;
    if ((digit & 0x80) == 0) {
      *length = value;
      return true;
    }
    multiplier *= 128;
  }
  return false;
}

bool mqttDiscard(uint32_t length)
{
  uint8_t discardBuffer[32];
  while (length > 0) {
    const size_t chunk = min<uint32_t>(length, sizeof(discardBuffer));
    if (!mqttReadBytes(discardBuffer, chunk, kMqttPacketTimeoutMs)) {
      return false;
    }
    length -= chunk;
  }
  return true;
}

bool mqttSendConnect()
{
  uint8_t payload[256];
  size_t length = 0;
  String clientId = "codex-c6-";
  clientId += WiFi.macAddress();
  clientId.replace(":", "");

  if (!mqttAppendUtf8(payload, sizeof(payload), &length, "MQTT")) {
    return false;
  }
  payload[length++] = 4;       // MQTT 3.1.1
  payload[length++] = 0xC2;    // clean session + username + password
  payload[length++] = static_cast<uint8_t>(kMqttKeepAliveSeconds >> 8);
  payload[length++] = static_cast<uint8_t>(kMqttKeepAliveSeconds & 0xFF);
  if (!mqttAppendUtf8(payload, sizeof(payload), &length, clientId.c_str()) ||
      !mqttAppendUtf8(payload, sizeof(payload), &length, kMqttUsername) ||
      !mqttAppendUtf8(payload, sizeof(payload), &length, kMqttPassword)) {
    return false;
  }
  return mqttSendPacket(0x10, payload, length);
}

bool mqttWaitForConnAck()
{
  uint8_t header = 0;
  uint32_t remaining = 0;
  uint8_t response[2] = {};
  if (!mqttReadByte(&header, kMqttPacketTimeoutMs) ||
      !mqttReadRemainingLength(&remaining) || header != 0x20 ||
      remaining != sizeof(response) ||
      !mqttReadBytes(response, sizeof(response), kMqttPacketTimeoutMs)) {
    return false;
  }
  return response[1] == 0;
}

bool mqttSendSubscribe()
{
  uint8_t payload[128];
  size_t length = 0;
  ++mqttPacketId;
  if (mqttPacketId == 0) mqttPacketId = 1;
  payload[length++] = static_cast<uint8_t>(mqttPacketId >> 8);
  payload[length++] = static_cast<uint8_t>(mqttPacketId & 0xFF);
  if (!mqttAppendUtf8(payload, sizeof(payload), &length, kMqttTopic)) {
    return false;
  }
  payload[length++] = 0;  // QoS 0
  return mqttSendPacket(0x82, payload, length);
}

bool mqttSendPing()
{
  return mqttSendPacket(0xC0, nullptr, 0);
}

bool mqttSendPubAck(uint16_t packetId)
{
  const uint8_t payload[] = {
      static_cast<uint8_t>(packetId >> 8),
      static_cast<uint8_t>(packetId & 0xFF),
  };
  return mqttSendPacket(0x40, payload, sizeof(payload));
}

void applyMqttSnapshot(const char *payload)
{
  UsageSnapshot parsed;
  if (!parseSnapshot(payload, &parsed)) {
    USBSerial.println("CODEX_MQTT_NACK error=invalid_v1_snapshot");
    return;
  }

  if (currentSnapshot.valid && parsed.capturedAt >= 0 &&
      parsed.capturedAt == currentSnapshot.capturedAt) {
    USBSerial.println("CODEX_MQTT_SNAPSHOT duplicate_retained=1");
    return;
  }

  currentSnapshot = parsed;
  renderSnapshot();
  USBSerial.print("CODEX_MQTT_SNAPSHOT remaining=");
  USBSerial.print(currentSnapshot.remaining);
  USBSerial.print(" windowMins=");
  USBSerial.print(currentSnapshot.windowMins);
  USBSerial.print(" plan=");
  USBSerial.println(currentSnapshot.planLabel);
}

bool mqttHandlePublish(uint8_t header, uint32_t remaining)
{
  if (remaining < 2) return false;

  uint8_t topicLengthBytes[2] = {};
  if (!mqttReadBytes(topicLengthBytes, sizeof(topicLengthBytes),
                     kMqttPacketTimeoutMs)) {
    return false;
  }
  const uint16_t topicLength =
      (static_cast<uint16_t>(topicLengthBytes[0]) << 8) | topicLengthBytes[1];
  remaining -= 2;

  char topic[96];
  if (topicLength >= sizeof(topic) || topicLength > remaining) {
    return mqttDiscard(remaining);
  }
  if (!mqttReadBytes(reinterpret_cast<uint8_t *>(topic), topicLength,
                     kMqttPacketTimeoutMs)) {
    return false;
  }
  topic[topicLength] = '\0';
  remaining -= topicLength;

  const uint8_t qos = (header >> 1) & 0x03;
  if (qos == 3) return false;
  uint16_t packetId = 0;
  if (qos > 0) {
    uint8_t packetIdBytes[2] = {};
    if (remaining < sizeof(packetIdBytes) ||
        !mqttReadBytes(packetIdBytes, sizeof(packetIdBytes),
                       kMqttPacketTimeoutMs)) {
      return false;
    }
    packetId = (static_cast<uint16_t>(packetIdBytes[0]) << 8) |
               packetIdBytes[1];
    remaining -= sizeof(packetIdBytes);
  }

  if (remaining >= kMqttPayloadCapacity) {
    USBSerial.println("CODEX_MQTT_NACK error=payload_too_large");
    return mqttDiscard(remaining);
  }
  if (!mqttReadBytes(reinterpret_cast<uint8_t *>(mqttPayload), remaining,
                     kMqttPacketTimeoutMs)) {
    return false;
  }
  mqttPayload[remaining] = '\0';

  if (strcmp(topic, kMqttTopic) == 0) {
    applyMqttSnapshot(mqttPayload);
  }
  return qos == 0 || mqttSendPubAck(packetId);
}

bool mqttProcessPackets()
{
  while (mqttClient.connected() && mqttClient.available() > 0) {
    uint8_t header = 0;
    uint32_t remaining = 0;
    if (!mqttReadByte(&header, kMqttPacketTimeoutMs) ||
        !mqttReadRemainingLength(&remaining)) {
      return false;
    }

    switch (header >> 4) {
      case 3:
        if (!mqttHandlePublish(header, remaining)) return false;
        break;
      case 13:  // PINGRESP
        if (!mqttDiscard(remaining)) return false;
        break;
      default:
        if (!mqttDiscard(remaining)) return false;
        break;
    }
  }
  return mqttClient.connected();
}

void markMqttDisconnected()
{
  if (mqttReported) {
    USBSerial.println("CODEX_MQTT_DISCONNECTED");
  }
  mqttReported = false;
  mqttClient.stop();
  nextMqttAttemptAt = millis() + kMqttRetryIntervalMs;
}

bool mqttConnectAndSubscribe()
{
  if (WiFi.status() != WL_CONNECTED) return false;

  mqttClient.stop();
  mqttClient.setTimeout(kMqttPacketTimeoutMs);
  if (!mqttClient.connect(kMqttHost, kMqttPort) || !mqttSendConnect() ||
      !mqttWaitForConnAck() || !mqttSendSubscribe()) {
    markMqttDisconnected();
    return false;
  }

  mqttReported = true;
  nextMqttRefreshAt = millis() + kMqttRefreshIntervalMs;
  USBSerial.println("CODEX_MQTT_CONNECTED");
  return true;
}

void maintainMqtt()
{
  if (!mqttClient.connected()) {
    mqttReported = false;
    return;
  }

  if (!mqttProcessPackets()) {
    markMqttDisconnected();
    return;
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextMqttRefreshAt) >= 0) {
    if (!mqttSendSubscribe()) {
      markMqttDisconnected();
      return;
    }
    nextMqttRefreshAt = now + kMqttRefreshIntervalMs;
    USBSerial.println("CODEX_MQTT_REFRESH_REQUEST interval=1800");
  }

  if (now - lastMqttActivityAt >=
      (kMqttKeepAliveSeconds * 1000UL) / 2UL) {
    if (!mqttSendPing()) {
      markMqttDisconnected();
    }
  }
}

void reportWiFiConnected()
{
  if (wifiReported) return;
  wifiReported = true;
  USBSerial.print("CODEX_WIFI_CONNECTED ip=");
  USBSerial.print(WiFi.localIP());
  USBSerial.print(" rssi=");
  USBSerial.println(WiFi.RSSI());
}

void startWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(kWifiSsid, kWifiPassword);
  wifiStarted = true;
  nextWifiAttemptAt = millis() + kWifiRetryIntervalMs;
  USBSerial.println("CODEX_WIFI_CONNECTING ssid_configured=1");
}

void serviceOta()
{
  if (strlen(kOtaPasswordHash) != 64) return;  // Never start unauthenticated OTA.
  if (WiFi.status() != WL_CONNECTED) {
    if (otaStarted) ArduinoOTA.end();
    otaStarted = false;
    return;
  }
  if (!otaStarted) {
    if (!esp_ota_get_next_update_partition(nullptr)) return;
    ArduinoOTA.setHostname(kOtaHostname);
    ArduinoOTA.setPort(kOtaPort);
    ArduinoOTA.setPasswordHash(kOtaPasswordHash);
    ArduinoOTA.onStart([]() {
      otaInProgress = true;
      lastOtaProgress = -1;
      mqttClient.stop();
      mqttReported = false;
      USBSerial.println("CODEX_OTA_START");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      const int percent = total ? static_cast<int>((uint64_t(progress) * 100) / total) : 0;
      if (percent / 10 != lastOtaProgress / 10 || lastOtaProgress < 0) {
        lastOtaProgress = percent;
        USBSerial.printf("CODEX_OTA_PROGRESS percent=%d\n", percent);
      }
    });
    ArduinoOTA.onEnd([]() {
      USBSerial.println("CODEX_OTA_COMPLETE reboot=1");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      otaInProgress = false;
      nextMqttAttemptAt = millis();
      USBSerial.printf("CODEX_OTA_ERROR code=%u\n", static_cast<unsigned>(error));
    });
    ArduinoOTA.begin();
    otaStarted = true;
    USBSerial.printf("CODEX_OTA_READY host=%s port=%u auth=required\n", kOtaHostname, kOtaPort);
  }
  ArduinoOTA.handle();
}

void waitForWiFiAtBoot()
{
  startWiFi();
  const uint32_t deadline = millis() + 30000UL;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    reportWiFiConnected();
  } else {
    USBSerial.println("CODEX_WIFI_PENDING retry=10s");
  }
}

void serviceNetwork()
{
  if (!wifiStarted) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (mqttClient.connected() || mqttReported) {
      markMqttDisconnected();
    }
    wifiReported = false;
    if (static_cast<int32_t>(millis() - nextWifiAttemptAt) >= 0) {
      WiFi.begin(kWifiSsid, kWifiPassword);
      nextWifiAttemptAt = millis() + kWifiRetryIntervalMs;
      USBSerial.println("CODEX_WIFI_RETRY interval=10s");
    }
    return;
  }

  reportWiFiConnected();
  if (!mqttClient.connected()) {
    if (static_cast<int32_t>(millis() - nextMqttAttemptAt) >= 0) {
      nextMqttAttemptAt = millis() + kMqttRetryIntervalMs;
      mqttConnectAndSubscribe();
    }
    return;
  }
  maintainMqtt();
}

uint16_t progressColor(int remaining)
{
  if (remaining <= 15) return kRed;
  if (remaining < 60) return kAmber;
  return kGreen;
}

void drawCentered(const char *text, int16_t y, uint8_t size, uint16_t color)
{
  const int16_t estimatedWidth = static_cast<int16_t>(strlen(text) * 6 * size);
  const int16_t x = max<int16_t>(0, (LCD_WIDTH - estimatedWidth) / 2);
  gfx->setTextSize(size);
  gfx->setTextColor(color, kBackground);
  gfx->setCursor(x, y);
  gfx->print(text);
}

const char *quotaHealth(int remaining)
{
  if (remaining >= 60) return "PLENTY";
  if (remaining >= 36) return "PAY ATTENTION";
  if (remaining >= 16) return "RUNNING LOW";
  return "ALMOST OUT";
}

uint32_t expectedPollSeconds(const UsageSnapshot &snapshot)
{
  return snapshot.nextPollIn > 0
           ? static_cast<uint32_t>(snapshot.nextPollIn)
           : kDefaultPollSeconds;
}

uint32_t cachedAfterSeconds(const UsageSnapshot &snapshot)
{
  return expectedPollSeconds(snapshot) + kFreshnessGraceSeconds;
}

uint32_t offlineAfterSeconds(const UsageSnapshot &snapshot)
{
  return expectedPollSeconds(snapshot) * 3UL;
}

const char *linkLabel(const UsageSnapshot &snapshot, uint32_t ageSeconds)
{
  if (ageSeconds >= offlineAfterSeconds(snapshot) ||
      snapshot.linkState == kLinkOffline) {
    return "OFFLINE";
  }
  if (ageSeconds >= cachedAfterSeconds(snapshot) ||
      snapshot.linkState == kLinkCached) {
    return "CACHED";
  }
  return "LIVE";
}

uint16_t linkColor(const UsageSnapshot &snapshot, uint32_t ageSeconds)
{
  const char *label = linkLabel(snapshot, ageSeconds);
  if (strcmp(label, "OFFLINE") == 0) return kRed;
  if (strcmp(label, "CACHED") == 0) return kAmber;
  return kGreen;
}

void drawStatusPill(const char *label, uint16_t color)
{
  const int16_t width = static_cast<int16_t>(strlen(label) * 6 * 2 + 34);
  const int16_t x = LCD_WIDTH - width - 24;
  const int16_t y = 18;
  gfx->fillRoundRect(x, y, width, 32, 16, kCard);
  gfx->drawRoundRect(x, y, width, 32, 16, color);
  gfx->fillCircle(x + 13, y + 16, 4, color);
  drawTextAt(label, x + 24, y + 9, 2, color);
}

void formatFreshness(uint32_t ageSeconds, char *target, size_t targetSize)
{
  if (ageSeconds < 60) {
    snprintf(target, targetSize, "UPDATED JUST NOW");
  } else if (ageSeconds < 3600) {
    snprintf(target, targetSize, "UPDATED %lum AGO",
             static_cast<unsigned long>(ageSeconds / 60));
  } else {
    snprintf(target, targetSize, "UPDATED %luH AGO",
             static_cast<unsigned long>(ageSeconds / 3600));
  }
}

void formatNextSync(const UsageSnapshot &snapshot, uint32_t ageSeconds,
                    char *target, size_t targetSize)
{
  if (ageSeconds >= offlineAfterSeconds(snapshot)) {
    snprintf(target, targetSize, "HOST OFFLINE / %s", snapshot.source);
    return;
  }
  if (snapshot.nextPollIn < 0) {
    snprintf(target, targetSize, "USB CDC SNAPSHOT / %s", snapshot.source);
    return;
  }
  const long remaining = snapshot.nextPollIn - static_cast<long>(ageSeconds);
  if (remaining <= 0) {
    snprintf(target, targetSize, "SYNC DUE / %s", snapshot.source);
  } else if (remaining >= 3600) {
    snprintf(target, targetSize, "NEXT SYNC %ldH %02ldM / %s",
             remaining / 3600, (remaining % 3600) / 60, snapshot.source);
  } else {
    snprintf(target, targetSize, "NEXT SYNC %ldM / %s",
             max(1L, remaining / 60), snapshot.source);
  }
}

void formatReset(long seconds, char *target, size_t targetSize)
{
  if (seconds < 0) {
    snprintf(target, targetSize, "RESET TIME --");
    return;
  }

  const long days = seconds / (24 * 3600);
  const long hours = (seconds % (24 * 3600)) / 3600;
  const long minutes = (seconds % 3600) / 60;
  if (days > 0) {
    snprintf(target, targetSize, "RESETS IN %ldD %ldH", days, hours);
  } else if (hours > 0) {
    snprintf(target, targetSize, "RESETS IN %ldH %02ldM", hours, minutes);
  } else {
    snprintf(target, targetSize, "RESETS IN %02ldM", minutes);
  }
}

uint8_t coverageAlpha(float coverage)
{
  if (coverage <= 0.0f) return 0;
  if (coverage >= 1.0f) return 255;
  return static_cast<uint8_t>(lroundf(coverage * 255.0f));
}

uint16_t blend565(uint16_t background, uint16_t foreground, uint8_t alpha)
{
  if (alpha == 0) return background;
  if (alpha == 255) return foreground;

  const uint16_t inverse = 255 - alpha;
  const uint16_t red = (((background >> 11) & 0x1F) * inverse +
                        ((foreground >> 11) & 0x1F) * alpha + 127) / 255;
  const uint16_t green = (((background >> 5) & 0x3F) * inverse +
                          ((foreground >> 5) & 0x3F) * alpha + 127) / 255;
  const uint16_t blue = ((background & 0x1F) * inverse +
                         (foreground & 0x1F) * alpha + 127) / 255;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

void drawUsageRing(int remaining, uint16_t color)
{
  constexpr float bitmapCenter = kRingOuterRadius + kRingBitmapMargin;
  constexpr float startAngle = 270.0f;
  const float sweepDegrees = 360.0f * clampPercent(remaining) / 100.0f;
  const float startRadians = startAngle * PI / 180.0f;
  const float endRadians = (startAngle + sweepDegrees) * PI / 180.0f;
  const float startCapX = cosf(startRadians) * kRingMiddleRadius;
  const float startCapY = sinf(startRadians) * kRingMiddleRadius;
  const float endCapX = cosf(endRadians) * kRingMiddleRadius;
  const float endCapY = sinf(endRadians) * kRingMiddleRadius;

  for (int16_t y = 0; y < kRingBitmapSize; ++y) {
    const float dy = y - bitmapCenter;
    for (int16_t x = 0; x < kRingBitmapSize; ++x) {
      const float dx = x - bitmapCenter;
      const float radius = sqrtf(dx * dx + dy * dy);
      const uint8_t ringAlpha = coverageAlpha(
          kRingStrokeRadius + 0.5f - fabsf(radius - kRingMiddleRadius));
      uint16_t pixel = blend565(kBackground, kPanel, ringAlpha);

      uint8_t progressAlpha = 0;
      if (remaining >= 100) {
        progressAlpha = ringAlpha;
      } else if (remaining > 0) {
        float angle = atan2f(dy, dx) * 180.0f / PI - startAngle;
        while (angle < 0.0f) angle += 360.0f;
        while (angle >= 360.0f) angle -= 360.0f;
        const float endEdgePixels =
            (sweepDegrees - angle) * kRingMiddleRadius * PI / 180.0f;
        const uint8_t endEdgeAlpha = coverageAlpha(endEdgePixels + 0.5f);
        progressAlpha = min(ringAlpha, endEdgeAlpha);

        const float startCapDistance =
            sqrtf((dx - startCapX) * (dx - startCapX) +
                  (dy - startCapY) * (dy - startCapY));
        const float endCapDistance =
            sqrtf((dx - endCapX) * (dx - endCapX) +
                  (dy - endCapY) * (dy - endCapY));
        progressAlpha = max(progressAlpha,
                            coverageAlpha(kRingStrokeRadius + 0.5f -
                                          startCapDistance));
        progressAlpha = max(progressAlpha,
                            coverageAlpha(kRingStrokeRadius + 0.5f -
                                          endCapDistance));
      }

      ringBitmap[y * kRingBitmapSize + x] =
          blend565(pixel, color, progressAlpha);
    }
  }

  gfx->draw16bitRGBBitmap(kRingCenterX - kRingOuterRadius - kRingBitmapMargin,
                          kRingCenterY - kRingOuterRadius - kRingBitmapMargin,
                          ringBitmap, kRingBitmapSize, kRingBitmapSize);
}

void formatWindow(int minutes, char *target, size_t targetSize)
{
  if (minutes == 7 * 24 * 60) {
    snprintf(target, targetSize, "7-DAY LIMIT");
  } else if (minutes > 0 && minutes % (24 * 60) == 0) {
    snprintf(target, targetSize, "%dD WINDOW", minutes / (24 * 60));
  } else if (minutes > 0 && minutes % 60 == 0) {
    snprintf(target, targetSize, "%dH WINDOW", minutes / 60);
  } else if (minutes > 0) {
    snprintf(target, targetSize, "%dM WINDOW", minutes);
  } else {
    snprintf(target, targetSize, "CODEX WINDOW");
  }
}

const char *displayPlanName(const char *plan)
{
  // Translate a reported code; never infer a usage multiplier from bare "pro".
  // Explicit planLabel names still take precedence over plan in parseSnapshot().
  struct PlanName { const char *code; const char *name; };
  static const PlanName names[] = {
      {"prolite", "Pro5X"}, {"pro5x", "Pro5X"},
      {"pro10x", "Pro10X"}, {"pro20x", "Pro20X"},
      {"pro50x", "Pro50X"}, {"pro", "Pro"},
      {"plus", "Plus"}, {"free", "Free"}, {"go", "Go"},
      {"team", "Team"}, {"business", "Business"},
      {"self_serve_business_prolite", "Business"},
      {"self_serve_business_usage_based", "Business"},
      {"ent26", "Enterprise"}, {"enterprise", "Enterprise"},
      {"enterprise_cbp_automation", "Enterprise"},
      {"enterprise_cbp_usage_based", "Enterprise"},
      {"edu", "Edu"}, {"unknown", "UNKNOWN"},
  };
  for (const auto &entry : names) {
    if (strcasecmp(plan, entry.code) == 0) return entry.name;
  }
  return plan;  // Preserve a future plan's supplied name instead of guessing.
}

void renderWaiting()
{
  if (!displayReady) return;

  gfx->fillScreen(kBackground);
  drawCentered("CodeX / PLAN PENDING", 12, 2, kText);
  drawCentered("MODEL PENDING", 38, 1, kMuted);
  drawCentered("WAITING", 58, 1, kBlue);
  drawUsageRing(0, kBlue);
  drawCentered("--%", 174, 6, kTertiary);
  drawCentered("WAITING FOR MQTT", 246, 2, kBlue);
  drawCentered("RETAINED SNAPSHOT", 350, 2, kMuted);
  drawCentered("HOME/CODEX/USAGE/C6", 384, 2, kTertiary);
}

void renderSnapshot()
{
  if (!displayReady || !currentSnapshot.valid) return;

  const long elapsed = static_cast<long>((millis() - currentSnapshot.receivedAt) / 1000);
  const long resetSeconds = currentSnapshot.resetsIn < 0
                                ? -1
                                : max(0L, currentSnapshot.resetsIn - elapsed);
  const uint16_t color = progressColor(currentSnapshot.remaining);
  const uint32_t ageSeconds = (millis() - currentSnapshot.receivedAt) / 1000;
  const uint16_t stateColor = linkColor(currentSnapshot, ageSeconds);

  char percentage[8];
  char resetText[32];
  char windowText[24];
  char freshnessText[32];
  char syncText[48];
  char healthText[20];
  char modelText[sizeof(currentSnapshot.modelLabel)];
  char planText[sizeof(currentSnapshot.planLabel) + 8];
  snprintf(percentage, sizeof(percentage), "%d%%", currentSnapshot.remaining);
  formatReset(resetSeconds, resetText, sizeof(resetText));
  formatWindow(currentSnapshot.windowMins, windowText, sizeof(windowText));
  formatFreshness(ageSeconds, freshnessText, sizeof(freshnessText));
  formatNextSync(currentSnapshot, ageSeconds, syncText, sizeof(syncText));
  snprintf(healthText, sizeof(healthText), "%s", quotaHealth(currentSnapshot.remaining));
  snprintf(modelText, sizeof(modelText), "%s", currentSnapshot.modelLabel);
  snprintf(planText, sizeof(planText), "CodeX %s", displayPlanName(currentSnapshot.planLabel));

  gfx->fillScreen(kBackground);
  drawCentered(planText, 12, strlen(planText) > 28 ? 1 : 2, kText);
  drawCentered(modelText, 36, 2, kMuted);
  drawCentered(linkLabel(currentSnapshot, ageSeconds), 58, 1, stateColor);

  drawUsageRing(currentSnapshot.remaining, color);

  const int16_t percentageWidth = static_cast<int16_t>(strlen(percentage) * 6 * 6);
  gfx->setTextSize(6);
  gfx->setTextColor(kText, kBackground);
  gfx->setCursor(max<int16_t>(0, kRingCenterX - percentageWidth / 2),
                 kRingCenterY - 62);
  gfx->print(percentage);

  drawCentered("REMAINING", 198, 2, kMuted);
  drawCentered(healthText, 224, 2, color);
  drawCentered(windowText, 264, 2, kMuted);
  drawCentered(resetText, 358, 2, kText);
  drawCentered(freshnessText, 400, 1, stateColor);
  drawCentered(syncText, 420, 1, kTertiary);
  gfx->fillRoundRect(kRingCenterX - 17, 451, 18, 6, 3, color);
  gfx->fillCircle(kRingCenterX + 13, 454, 4, kTertiary);

  lastRenderAt = millis();
}

void sendAck(const UsageSnapshot &snapshot)
{
  USBSerial.print("CODEX_ACK v=1 remaining=");
  USBSerial.print(snapshot.remaining);
  USBSerial.print(" windowMins=");
  USBSerial.print(snapshot.windowMins);
  USBSerial.print(" nextPollIn=");
  USBSerial.print(snapshot.nextPollIn);
  USBSerial.print(" plan=");
  USBSerial.print(snapshot.planLabel);
  USBSerial.print(" display=");
  USBSerial.print(displayPlanName(snapshot.planLabel));
  USBSerial.println();
}

void processLine()
{
  inputLine[inputLength] = '\0';
  if (strcmp(inputLine, "CODEX_DIAG") == 0) {
    reportDiagnostics();
    inputLength = 0;
    inputOverflow = false;
    return;
  }

  UsageSnapshot parsed;
  if (parseSnapshot(inputLine, &parsed)) {
    currentSnapshot = parsed;
    renderSnapshot();
    sendAck(currentSnapshot);
  } else {
    USBSerial.println("CODEX_NACK error=invalid_v1_snapshot");
  }
  inputLength = 0;
  inputOverflow = false;
}

void readSerialSnapshot()
{
  while (USBSerial.available() > 0) {
    const char value = static_cast<char>(USBSerial.read());
    if (value == '\n') {
      if (inputOverflow) {
        USBSerial.println("CODEX_NACK error=payload_too_large");
        inputLength = 0;
        inputOverflow = false;
      } else if (inputLength > 0) {
        processLine();
      }
      continue;
    }
    if (value == '\r') {
      continue;
    }
    if (inputLength + 1 >= sizeof(inputLine)) {
      inputOverflow = true;
      continue;
    }
    inputLine[inputLength++] = value;
  }
}

void scanI2cForDiagnostics()
{
  USBSerial.println("CODEX_I2C_SCAN_BEGIN");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      USBSerial.print("CODEX_I2C_FOUND address=0x");
      if (address < 16) {
        USBSerial.print('0');
      }
      USBSerial.println(address, HEX);
    }
  }
  USBSerial.println("CODEX_I2C_SCAN_END");
}

void reportDiagnostics()
{
  USBSerial.println("CODEX_DIAG_BEGIN");
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  USBSerial.printf("CODEX_FIRMWARE build=%s_%s partition=%s\n", __DATE__, __TIME__,
                   running ? running->label : "unknown");
  USBSerial.printf("CODEX_OTA_STATE ready=%u configured=%u port=%u next=%s capacity=%lu\n",
                   otaStarted, strlen(kOtaPasswordHash) == 64, kOtaPort,
                   next ? next->label : "none", next ? static_cast<unsigned long>(next->size) : 0UL);
  USBSerial.print("CODEX_NETWORK ip=");
  USBSerial.println(WiFi.localIP());
  USBSerial.printf("CODEX_PLAN valid=%u label=%s display=%s\n", currentSnapshot.valid,
                   currentSnapshot.planLabel, displayPlanName(currentSnapshot.planLabel));
  USBSerial.println("CODEX_BOARD model=ESP32-C6-Touch-AMOLED-1.43 display=co5300 resolution=466x466");
  USBSerial.println("CODEX_UI renderer=aa-bitmap-ring-v3 refresh_ms=300000");
  USBSerial.println("CODEX_I2C_CONFIG sda=18 scl=8");
  USBSerial.print("CODEX_STATE expander=");
  USBSerial.print(expanderReady ? "ready" : "failed");
  USBSerial.print(" lcd_enable=");
  USBSerial.print(lcdEnableReady ? "ready" : "failed");
  USBSerial.print(" display_init=");
  USBSerial.println(displayReady ? "ready" : "failed");
  scanI2cForDiagnostics();
  USBSerial.println("CODEX_DIAG_END");
}

}  // namespace

void setup()
{
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);
  delay(100);

  USBSerial.println("CODEX_BOARD model=ESP32-C6-Touch-AMOLED-1.43 display=co5300 resolution=466x466");
  USBSerial.println("CODEX_I2C_CONFIG sda=18 scl=8");

  // Network setup intentionally comes before MQTT and display data handling.
  waitForWiFiAtBoot();

  Wire.begin(IIC_SDA, IIC_SCL);
  expanderReady = expander.begin(IO_EXPANDER_ADDRESS);
  if (!expanderReady) {
    USBSerial.println("CODEX_ERROR expander=not_found");
  } else {
    const bool pinConfigured = expander.pinMode(LCD_ENABLE_EXIO, OUTPUT);
    const bool displayEnabled = expander.digitalWrite(LCD_ENABLE_EXIO, 1);
    lcdEnableReady = pinConfigured && displayEnabled;
    if (lcdEnableReady) {
      USBSerial.println("CODEX_EXPANDER_READY address=0x20 lcd_enable=1");
    } else {
      USBSerial.println("CODEX_ERROR expander=lcd_enable_failed");
    }
  }
  scanI2cForDiagnostics();
  delay(20);

  displayReady = gfx->begin(40000000);
  if (!displayReady) {
    USBSerial.println("CODEX_ERROR display=init_failed");
  } else {
    gfx->setBrightness(220);
    renderWaiting();
    USBSerial.println("CODEX_DISPLAY_READY driver=co5300 qspi_hz=40000000");
  }

  USBSerial.println("CODEX_METER_READY v=1 transport=mqtt display=co5300 board=amoled-1.43 ui=aa-bitmap-ring-v3 refresh_s=1800");
  if (WiFi.status() == WL_CONNECTED) {
    serviceOta();
    mqttConnectAndSubscribe();
  }
}

void loop()
{
  serviceOta();
  if (otaInProgress) {
    delay(5);
    return;
  }
  serviceNetwork();
  readSerialSnapshot();

  if (currentSnapshot.valid &&
      millis() - lastRenderAt >= kMinimumRenderIntervalMs) {
    renderSnapshot();
  }
  delay(5);
}
