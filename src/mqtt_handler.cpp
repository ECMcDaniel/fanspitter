#include "mqtt_handler.h"
#include <ArduinoJson.h>

// ────────────────────────────────────────────────────────────────────────────
// Topic constants
// ────────────────────────────────────────────────────────────────────────────
static constexpr const char* TOPIC_LEARN   = "fanspitter/learn";
static constexpr const char* TOPIC_CMD     = "fanspitter/cmd";
static constexpr const char* TOPIC_STATUS  = "fanspitter/status";
static constexpr const char* TOPIC_LEARNED = "fanspitter/learned";

// ────────────────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────────────────
MQTTHandler* MQTTHandler::_instance = nullptr;

// ────────────────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────────────────
MQTTHandler::MQTTHandler(const char* broker, uint16_t port)
  : _broker(broker),
    _port(port),
    _lastReconnectMs(0)
{
  _client.setClient(_wifiClient);
  _client.setServer(_broker, _port);
  _client.setCallback(mqttCallback);
  _client.setBufferSize(512);  // large enough for status JSON with 8 slots
  _instance = this;
}

// ────────────────────────────────────────────────────────────────────────────
// begin()
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::begin(const char* clientId, SlotCb onLearn, SlotCb onSend) {
  _clientId = clientId;
  _onLearn  = onLearn;
  _onSend   = onSend;
  reconnect();
}

// ────────────────────────────────────────────────────────────────────────────
// loop()
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::loop() {
  if (!_client.connected()) {
    unsigned long now = millis();
    if (now - _lastReconnectMs > 5000UL) {
      _lastReconnectMs = now;
      reconnect();
    }
    return;
  }
  _client.loop();
}

// ────────────────────────────────────────────────────────────────────────────
// publishStatus()
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::publishStatus(const char* state,
                                const String names[], uint8_t count) {
  if (!_client.connected()) return;

  JsonDocument doc;
  doc["state"] = state;
  doc["ip"]    = WiFi.localIP().toString();
  doc["rssi"]  = (int)WiFi.RSSI();

  JsonArray arr = doc["slots"].to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    if (names[i].length() > 0) {
      arr.add(names[i]);
    }
  }

  String json;
  serializeJson(doc, json);
  _client.publish(TOPIC_STATUS, json.c_str(), /*retain=*/true);
}

// ────────────────────────────────────────────────────────────────────────────
// publishLearned()
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::publishLearned(const String& slot, uint16_t pulseCount) {
  if (!_client.connected()) return;

  JsonDocument doc;
  doc["slot"]   = slot;
  doc["pulses"] = pulseCount;

  String json;
  serializeJson(doc, json);
  _client.publish(TOPIC_LEARNED, json.c_str());
}

// ────────────────────────────────────────────────────────────────────────────
// isConnected()
// ────────────────────────────────────────────────────────────────────────────
bool MQTTHandler::isConnected() {
  return _client.connected();
}

// ────────────────────────────────────────────────────────────────────────────
// reconnect() — attempt a single connection; caller retries on failure
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::reconnect() {
  Serial.printf("[MQTT] Connecting to %s:%u as %s\n",
                _broker, _port, _clientId.c_str());

  bool ok = _client.connect(
    _clientId.c_str(),
    /*user*/         nullptr,
    /*pass*/         nullptr,
    /*willTopic*/    TOPIC_STATUS,
    /*willQos*/      0,
    /*willRetain*/   true,
    /*willMsg*/      "{\"state\":\"offline\"}"
  );

  if (ok) {
    Serial.println("[MQTT] Connected");
    subscribe();
    // Immediately publish online status (no slots known here; caller will re-publish)
    _client.publish(TOPIC_STATUS, "{\"state\":\"idle\"}", /*retain=*/true);
  } else {
    Serial.printf("[MQTT] Failed (rc=%d), retrying in 5 s\n",
                  _client.state());
  }
}

// ────────────────────────────────────────────────────────────────────────────
// subscribe()
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::subscribe() {
  _client.subscribe(TOPIC_LEARN);
  _client.subscribe(TOPIC_CMD);
  Serial.printf("[MQTT] Subscribed to %s, %s\n", TOPIC_LEARN, TOPIC_CMD);
}

// ────────────────────────────────────────────────────────────────────────────
// mqttCallback() — static, dispatches via singleton
// ────────────────────────────────────────────────────────────────────────────
void MQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!_instance) return;

  // Build a null-terminated string from the payload
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  String t(topic);
  Serial.printf("[MQTT] Rx %s => \"%s\"\n", topic, msg.c_str());

  if (t == TOPIC_LEARN && _instance->_onLearn) {
    _instance->_onLearn(msg);
  } else if (t == TOPIC_CMD && _instance->_onSend) {
    _instance->_onSend(msg);
  }
}
