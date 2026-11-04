#include "mqtt_handler.h"
#include <ArduinoJson.h>

static constexpr const char* OFFLINE_PAYLOAD = "{\"state\":\"offline\"}";
static constexpr uint32_t    BACKOFF_MIN_MS  = 5000;
static constexpr uint32_t    BACKOFF_MAX_MS  = 60000;

MQTTHandler* MQTTHandler::_instance = nullptr;

MQTTHandler::MQTTHandler(const MqttConfig& cfg) : _cfg(cfg) {
  String base = _cfg.baseTopic;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (base.length() == 0) base = "fanspitter";
  _tLearn   = base + "/learn";
  _tCmd     = base + "/cmd";
  _tStatus  = base + "/status";
  _tLearned = base + "/learned";

  _client.setClient(_wifiClient);
  _client.setServer(_cfg.host.c_str(), _cfg.port);
  _client.setCallback(mqttCallback);
  _client.setBufferSize(1024);   // status JSON with 8 × 32-char slot names
  _client.setSocketTimeout(3);   // bound how long connect() can block (s)
  _client.setKeepAlive(30);
  _instance = this;
}

void MQTTHandler::begin(const String& clientId, SlotCb onLearn, SlotCb onSend,
                        ConnectCb onConnect) {
  _clientId  = clientId;
  _onLearn   = onLearn;
  _onSend    = onSend;
  _onConnect = onConnect;
  _wifiClient.setConnectionTimeout(3000);  // ms; bounds the TCP connect
  if (enabled()) reconnect();
}

void MQTTHandler::loop() {
  if (!enabled()) return;
  if (_client.connected()) {
    _client.loop();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - _lastAttemptMs < _backoffMs) return;
  reconnect();
}

bool MQTTHandler::reconnect() {
  _lastAttemptMs = millis();
  Serial.printf("[MQTT] Connecting to %s:%u as %s%s\n",
                _cfg.host.c_str(), _cfg.port, _clientId.c_str(),
                _cfg.user.length() ? " (auth)" : "");

  const char* user = _cfg.user.length() ? _cfg.user.c_str() : nullptr;
  const char* pass = _cfg.user.length() ? _cfg.pass.c_str() : nullptr;

  bool ok = _client.connect(_clientId.c_str(), user, pass,
                            _tStatus.c_str(), /*willQos=*/0, /*willRetain=*/true,
                            OFFLINE_PAYLOAD);
  if (!ok) {
    // Exponential backoff so a dead broker doesn't stall the loop every 5 s.
    _backoffMs = _backoffMs ? min<uint32_t>(_backoffMs * 2, BACKOFF_MAX_MS) : BACKOFF_MIN_MS;
    Serial.printf("[MQTT] Failed (rc=%d), retrying in %lu s\n",
                  _client.state(), (unsigned long)(_backoffMs / 1000));
    return false;
  }

  _backoffMs = 0;
  Serial.println("[MQTT] Connected");
  _client.subscribe(_tLearn.c_str());
  _client.subscribe(_tCmd.c_str());
  Serial.printf("[MQTT] Subscribed to %s, %s\n", _tLearn.c_str(), _tCmd.c_str());
  if (_onConnect) _onConnect();
  return true;
}

bool MQTTHandler::publishStatus(const String& json) {
  if (!_client.connected()) return false;
  return _client.publish(_tStatus.c_str(), json.c_str(), /*retain=*/true);
}

bool MQTTHandler::publishLearned(const String& slot, bool ok, uint16_t pulseCount,
                                 const char* error) {
  if (!_client.connected()) return false;
  JsonDocument doc;
  doc["slot"] = slot;
  doc["ok"]   = ok;
  if (ok) doc["pulses"] = pulseCount;
  else    doc["error"]  = error ? error : "failed";
  String json;
  serializeJson(doc, json);
  return _client.publish(_tLearned.c_str(), json.c_str());
}

void MQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!_instance) return;

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  Serial.printf("[MQTT] Rx %s => \"%s\"\n", topic, msg.c_str());

  if (_instance->_tLearn == topic) {
    if (_instance->_onLearn) _instance->_onLearn(msg);
  } else if (_instance->_tCmd == topic) {
    if (_instance->_onSend) _instance->_onSend(msg);
  }
}
