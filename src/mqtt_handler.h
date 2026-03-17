#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

// ────────────────────────────────────────────────────────────────────────────
// MQTTHandler — PubSubClient wrapper with reconnect logic and LWT
//
// Topics:
//   Sub  fanspitter/learn  — payload = slot name (e.g. "fan1_high")
//   Sub  fanspitter/cmd    — payload = slot name to replay
//   Pub  fanspitter/status — JSON {"state":"...","slots":[...],"ip":"...","rssi":-60}
//   Pub  fanspitter/learned— JSON {"slot":"...","pulses":84}
// ────────────────────────────────────────────────────────────────────────────
class MQTTHandler {
public:
  using SlotCb = std::function<void(const String& slot)>;

  // broker: hostname or IP address string
  MQTTHandler(const char* broker, uint16_t port = 1883);

  // Call after WiFi is up.
  void begin(const char* clientId, SlotCb onLearn, SlotCb onSend);

  // Call every loop() iteration.
  void loop();

  // Publish fanspitter/status with current state and slot list.
  // names[] must have 'count' valid entries (can be called with count=0).
  void publishStatus(const char* state,
                     const String names[], uint8_t count);

  // Publish fanspitter/learned after a successful capture.
  void publishLearned(const String& slot, uint16_t pulseCount);

  bool isConnected();

private:
  const char*   _broker;
  uint16_t      _port;
  String        _clientId;
  WiFiClient    _wifiClient;
  PubSubClient  _client;
  unsigned long _lastReconnectMs;

  SlotCb _onLearn;
  SlotCb _onSend;

  // Singleton for the static PubSubClient callback (only one MQTTHandler instance)
  static MQTTHandler* _instance;
  static void mqttCallback(char* topic, byte* payload, unsigned int length);

  void reconnect();
  void subscribe();
};
