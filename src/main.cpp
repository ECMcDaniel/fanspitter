#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#ifdef HTTP_ANY
#undef HTTP_ANY
#endif
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "cc1101.h"
#include "rf_learn.h"
#include "signal_store.h"
#include "mqtt_handler.h"
#include "web_ui.h"

// ────────────────────────────────────────────────────────────────────────────
// Pin definitions
// ────────────────────────────────────────────────────────────────────────────
#define PIN_GDO0  2
#define PIN_CS    5

// ────────────────────────────────────────────────────────────────────────────
// Application state
// ────────────────────────────────────────────────────────────────────────────
enum class AppState { IDLE, LEARNING, TRANSMITTING };
static AppState       appState       = AppState::IDLE;
static unsigned long  learnStartMs   = 0;
static String         pendingLearnSlot;
static bool           servicesStarted = false;

// Shared TX buffer (used when replaying a stored signal)
static uint16_t txBuf[RF_MAX_PULSES];

// ────────────────────────────────────────────────────────────────────────────
// Configuration (MQTT broker / port stored in NVS namespace "fs_conf")
// ────────────────────────────────────────────────────────────────────────────
static char  cfgBroker[64] = "192.168.1.100";
static char  cfgPort[6]    = "1883";

static void loadConfig() {
  Preferences prefs;
  prefs.begin("fs_conf", true);
  String b = prefs.getString("broker", cfgBroker);
  String p = prefs.getString("port",   cfgPort);
  prefs.end();
  b.toCharArray(cfgBroker, sizeof(cfgBroker));
  p.toCharArray(cfgPort,   sizeof(cfgPort));
}

static void saveConfig(const char* broker, const char* port) {
  Preferences prefs;
  prefs.begin("fs_conf", false);
  prefs.putString("broker", broker);
  prefs.putString("port",   port);
  prefs.end();
}

// ────────────────────────────────────────────────────────────────────────────
// Core objects
// ────────────────────────────────────────────────────────────────────────────
static CC1101        cc1101(PIN_CS, PIN_GDO0);
static SignalStore   signalStore;
static RFLearn       rfLearn(cc1101, PIN_GDO0);
static AsyncWebServer server(80);
static WebUI         webUI(server, signalStore);
static WiFiManager   wm;

// MQTTHandler and WiFiManager params are created in setup() after config loads
static MQTTHandler*          mqttHandler = nullptr;
static WiFiManagerParameter* paramBroker = nullptr;
static WiFiManagerParameter* paramPort   = nullptr;

// ────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ────────────────────────────────────────────────────────────────────────────
static String buildStatusJson();
static void   cmdLearn(const String& slot);
static void   cmdSend(const String& slot);
static void   cmdDelete(const String& slot);
static void   startServices();
static void   publishStatusAll();

// ────────────────────────────────────────────────────────────────────────────
// setup()
// ────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[FanSpitter] booting...");

  // 1. SPI bus (VSPI: MOSI=23, MISO=19, SCK=18)
  SPI.begin();

  // 2. CC1101
  if (!cc1101.begin()) {
    Serial.println("[FATAL] CC1101 not responding — halting");
    while (true) delay(1000);
  }

  // 3. NVS signal storage
  signalStore.begin();

  // 4. RF ISR + RX mode
  rfLearn.begin();

  // 5. Callbacks from RF module
  rfLearn.onLearnDone([](bool ok, const String& reason) {
    if (ok) {
      uint16_t count = rfLearn.getConfirmedPulses(txBuf, RF_MAX_PULSES);
      if (count > 0) {
        signalStore.save(pendingLearnSlot, txBuf, count);
        if (mqttHandler) {
          mqttHandler->publishLearned(pendingLearnSlot, count);
        }
        Serial.printf("[App] Saved \"%s\" (%u pulses)\n",
                      pendingLearnSlot.c_str(), count);
      }
    } else {
      Serial.printf("[App] Learn failed: %s\n", reason.c_str());
    }
    appState = AppState::IDLE;
    publishStatusAll();
  });

  rfLearn.onTransmitDone([](bool ok, const String&) {
    (void)ok;
    appState = AppState::IDLE;
    publishStatusAll();
  });

  // 6. Load MQTT config from NVS
  loadConfig();

  // 7. WiFiManager — add custom MQTT parameters to captive portal
  paramBroker = new WiFiManagerParameter("broker", "MQTT Broker IP", cfgBroker, 63);
  paramPort   = new WiFiManagerParameter("port",   "MQTT Port",      cfgPort,    5);
  wm.addParameter(paramBroker);
  wm.addParameter(paramPort);
  wm.setHostname("fanspitter");

  // Save MQTT params when WiFiManager saves credentials
  wm.setSaveParamsCallback([]() {
    saveConfig(paramBroker->getValue(), paramPort->getValue());
    Serial.printf("[App] MQTT config saved: %s:%s\n",
                  paramBroker->getValue(), paramPort->getValue());
  });

  // Non-blocking: spawns an AP named "FanSpitter-Setup" if no saved credentials.
  // The captive portal closes after 120 s of inactivity if no client connects.
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(120);
  wm.autoConnect("FanSpitter-Setup");

  Serial.println("[App] setup done — entering loop");
}

// ────────────────────────────────────────────────────────────────────────────
// loop()
// ────────────────────────────────────────────────────────────────────────────
void loop() {
  // A. WiFiManager tick (drives captive portal DNS / HTTP in AP mode)
  wm.process();

  // B. Start web/MQTT services once WiFi connects (deferred)
  if (!servicesStarted && WiFi.status() == WL_CONNECTED) {
    startServices();
    servicesStarted = true;
  }

  // C. MQTT tick (reconnect + message dispatch)
  if (servicesStarted && mqttHandler) {
    mqttHandler->loop();
  }

  // D. RF state machine tick
  rfLearn.process();

  // E. Learn-mode timeout watchdog (belt-and-suspenders; rfLearn also has one)
  if (appState == AppState::LEARNING) {
    if ((millis() - learnStartMs) > (RF_LEARN_TIMEOUT_MS + 1000UL)) {
      Serial.println("[App] Learn watchdog fired");
      appState = AppState::IDLE;
      publishStatusAll();
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// startServices() — called once when WiFi first connects
// ────────────────────────────────────────────────────────────────────────────
static void startServices() {
  Serial.print("[App] WiFi connected: ");
  Serial.println(WiFi.localIP());

  // Reload MQTT config (may have been updated via portal during this boot)
  loadConfig();
  strncpy(cfgBroker, paramBroker->getValue(), sizeof(cfgBroker) - 1);
  strncpy(cfgPort,   paramPort->getValue(),   sizeof(cfgPort)   - 1);

  // Web UI
  webUI.begin(
    [](const String& s) { cmdLearn(s);  },
    [](const String& s) { cmdSend(s);   },
    [](const String& s) { cmdDelete(s); },
    []()                { return buildStatusJson(); }
  );
  server.begin();
  Serial.println("[App] HTTP server started");

  // MQTT — unique client ID based on MAC
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String clientId = "fanspitter-" + mac;

  uint16_t port = (uint16_t)atoi(cfgPort);
  if (port == 0) port = 1883;

  mqttHandler = new MQTTHandler(cfgBroker, port);
  mqttHandler->begin(
    clientId.c_str(),
    [](const String& s) { cmdLearn(s); },
    [](const String& s) { cmdSend(s);  }
  );
  Serial.printf("[App] MQTT handler started (broker=%s:%u)\n", cfgBroker, port);

  publishStatusAll();
}

// ────────────────────────────────────────────────────────────────────────────
// Command functions (called from both web UI and MQTT)
// ────────────────────────────────────────────────────────────────────────────
static void cmdLearn(const String& slot) {
  if (appState != AppState::IDLE) {
    Serial.printf("[App] Learn rejected: busy (%s)\n", slot.c_str());
    return;
  }
  if (slot.length() == 0) return;

  pendingLearnSlot = slot;
  if (rfLearn.startLearn()) {
    appState     = AppState::LEARNING;
    learnStartMs = millis();
    publishStatusAll();
  }
}

static void cmdSend(const String& slot) {
  if (appState != AppState::IDLE) {
    Serial.printf("[App] Send rejected: busy (%s)\n", slot.c_str());
    return;
  }

  uint16_t count = signalStore.load(slot, txBuf, RF_MAX_PULSES);
  if (count == 0) {
    Serial.printf("[App] Slot not found: \"%s\"\n", slot.c_str());
    return;
  }

  if (rfLearn.startTransmit(txBuf, count)) {
    appState = AppState::TRANSMITTING;
    publishStatusAll();
    // Note: rfLearn.startTransmit() is synchronous; appState is reset to
    // IDLE inside doTransmit() before the onTransmitDone callback fires.
  }
}

static void cmdDelete(const String& slot) {
  signalStore.remove(slot);
  publishStatusAll();
}

// ────────────────────────────────────────────────────────────────────────────
// buildStatusJson() — shared by /api/status and MQTT publishStatus
// ────────────────────────────────────────────────────────────────────────────
static String buildStatusJson() {
  JsonDocument doc;

  // State string
  switch (appState) {
    case AppState::LEARNING:     doc["state"] = "learning";     break;
    case AppState::TRANSMITTING: doc["state"] = "transmitting"; break;
    default:                     doc["state"] = "idle";         break;
  }

  // Learn countdown
  if (appState == AppState::LEARNING) {
    unsigned long elapsed = millis() - learnStartMs;
    long remaining = ((long)RF_LEARN_TIMEOUT_MS - (long)elapsed) / 1000L;
    doc["learnSecs"] = (remaining > 0) ? (int)remaining : 0;
  } else {
    doc["learnSecs"] = 0;
  }

  // Network
  doc["ip"]   = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "disconnected";
  doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
  doc["mqtt"] = (mqttHandler && mqttHandler->isConnected());

  // Slot list
  JsonArray arr = doc["slots"].to<JsonArray>();
  String names[SignalStore::MAX_SLOTS];
  signalStore.listNames(names);
  for (uint8_t i = 0; i < SignalStore::MAX_SLOTS; i++) {
    if (names[i].length() > 0) arr.add(names[i]);
  }

  String json;
  serializeJson(doc, json);
  return json;
}

// ────────────────────────────────────────────────────────────────────────────
// publishStatusAll() — push status to MQTT (if connected)
// ────────────────────────────────────────────────────────────────────────────
static void publishStatusAll() {
  if (!mqttHandler || !mqttHandler->isConnected()) return;

  const char* stateStr;
  switch (appState) {
    case AppState::LEARNING:     stateStr = "learning";     break;
    case AppState::TRANSMITTING: stateStr = "transmitting"; break;
    default:                     stateStr = "idle";         break;
  }

  String names[SignalStore::MAX_SLOTS];
  signalStore.listNames(names);
  uint8_t count = signalStore.count();
  mqttHandler->publishStatus(stateStr, names, count);
}
