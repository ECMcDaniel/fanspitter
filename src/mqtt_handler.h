#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

// ────────────────────────────────────────────────────────────────────────────
// MQTTHandler — PubSubClient wrapper with reconnect, auth and LWT
//
// Topics (<base> defaults to "fanspitter"):
//   Sub  <base>/learn    — payload = slot name to learn
//   Sub  <base>/cmd      — payload = slot name to replay
//   Pub  <base>/status   — retained JSON status; LWT {"state":"offline"}
//   Pub  <base>/learned  — JSON {"slot":"…","ok":true,"pulses":84}
//                          or   {"slot":"…","ok":false,"error":"timeout"}
//
// Must only be used from the loop task.
// ────────────────────────────────────────────────────────────────────────────
struct MqttConfig {
  String   host;        // empty = MQTT disabled
  uint16_t port = 1883;
  String   user;        // empty = anonymous
  String   pass;
  String   baseTopic = "fanspitter";
};

class MQTTHandler {
public:
  using SlotCb    = std::function<void(const String& slot)>;
  using ConnectCb = std::function<void()>;

  explicit MQTTHandler(const MqttConfig& cfg);

  // Call after WiFi is up. onConnect fires after every (re)connect so the
  // caller can publish fresh status.
  void begin(const String& clientId, SlotCb onLearn, SlotCb onSend, ConnectCb onConnect);

  // Call every loop() iteration.
  void loop();

  bool publishStatus(const String& json);
  bool publishLearned(const String& slot, bool ok, uint16_t pulseCount, const char* error);

  bool isConnected() { return _client.connected(); }
  bool enabled() const { return _cfg.host.length() > 0; }

private:
  MqttConfig    _cfg;
  String        _clientId;
  String        _tLearn, _tCmd, _tStatus, _tLearned;
  WiFiClient    _wifiClient;
  PubSubClient  _client;
  unsigned long _lastAttemptMs = 0;
  uint32_t      _backoffMs     = 0;

  SlotCb    _onLearn;
  SlotCb    _onSend;
  ConnectCb _onConnect;

  static MQTTHandler* _instance;
  static void mqttCallback(char* topic, byte* payload, unsigned int length);

  bool reconnect();
};
