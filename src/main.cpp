#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <atomic>

#include "config.h"
#include "commands.h"
#include "slot_name.h"
#include "cc1101.h"
#include "rf_learn.h"
#include "signal_store.h"
#include "mqtt_handler.h"
#include "web_ui.h"

// ────────────────────────────────────────────────────────────────────────────
// Threading model
//
//   loop task (core 1)  — owns the radio, RF state machine, MQTT client and
//                         all command execution.
//   AsyncTCP task       — web handlers. They only read thread-safe state
//                         (atomics, SignalStore) and enqueue commands.
//
// MQTT callbacks run inside mqtt->loop() on the loop task but go through the
// same queue so both front-ends behave identically.
// ────────────────────────────────────────────────────────────────────────────

enum class AppState : uint8_t { IDLE, LEARNING, TRANSMITTING };

struct Command {
  enum class Type : uint8_t { LEARN, SEND, DELETE } type;
  char slot[SLOT_NAME_MAX + 1];
};

static std::atomic<AppState> appState{AppState::IDLE};
static std::atomic<uint32_t> learnStartMs{0};
static std::atomic<bool>     mqttUp{false};
static bool                  rfOk = false;        // written once in setup()
static bool                  otaEnabled = false;  // written once in startServices()
static bool                  servicesStarted = false;
static volatile bool         statusDirty = true;
static volatile bool         restartPending = false;
static String                pendingLearnSlot;    // loop task only
// Copy of the slot being learned for the status JSON, which the web server
// task builds. Guarded by learnSlotMux.
static char                  learnSlotName[SLOT_NAME_MAX + 1] = "";
static portMUX_TYPE          learnSlotMux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t         cmdQueue = nullptr;

static uint16_t txBuf[RF_MAX_PULSES];             // loop task only

// ────────────────────────────────────────────────────────────────────────────
// Persistent settings (NVS namespace "fs_conf"), edited via the setup portal
// ────────────────────────────────────────────────────────────────────────────
struct Settings {
  char mqttHost[64]  = "";
  char mqttPort[6]   = "1883";
  char mqttUser[64]  = "";
  char mqttPass[64]  = "";
  char baseTopic[48] = "fanspitter";
  char adminPass[64] = "";   // OTA password; OTA is disabled while empty
};
static Settings settings;

static void loadSettings() {
  Preferences p;
  if (!p.begin("fs_conf", true)) return;  // first boot: keep defaults
  p.getString("broker", settings.mqttHost,  sizeof(settings.mqttHost));
  p.getString("port",   settings.mqttPort,  sizeof(settings.mqttPort));
  p.getString("user",   settings.mqttUser,  sizeof(settings.mqttUser));
  p.getString("pass",   settings.mqttPass,  sizeof(settings.mqttPass));
  p.getString("topic",  settings.baseTopic, sizeof(settings.baseTopic));
  p.getString("admin",  settings.adminPass, sizeof(settings.adminPass));
  p.end();
  if (settings.baseTopic[0] == '\0') strlcpy(settings.baseTopic, "fanspitter", sizeof(settings.baseTopic));
}

static void saveSettings() {
  Preferences p;
  p.begin("fs_conf", false);
  p.putString("broker", settings.mqttHost);
  p.putString("port",   settings.mqttPort);
  p.putString("user",   settings.mqttUser);
  p.putString("pass",   settings.mqttPass);
  p.putString("topic",  settings.baseTopic);
  p.putString("admin",  settings.adminPass);
  p.end();
}

// ────────────────────────────────────────────────────────────────────────────
// Core objects
// ────────────────────────────────────────────────────────────────────────────
static CC1101         cc1101(PIN_CS, PIN_GDO0, PIN_MISO);
static SignalStore    signalStore;
static RFLearn        rfLearn(cc1101, PIN_GDO0);
static AsyncWebServer server(80);
static WebUI          webUI(server);
static WiFiManager    wm;
static MQTTHandler*   mqtt = nullptr;

// Portal fields. Password fields are never pre-filled (the setup AP is open);
// leaving one blank keeps the stored value.
static WiFiManagerParameter pHost ("broker", "MQTT broker host/IP (blank = no MQTT)", "", sizeof(Settings::mqttHost) - 1);
static WiFiManagerParameter pPort ("port",   "MQTT port",                             "", sizeof(Settings::mqttPort) - 1);
static WiFiManagerParameter pUser ("user",   "MQTT username (optional)",              "", sizeof(Settings::mqttUser) - 1);
static WiFiManagerParameter pPass ("pass",   "MQTT password (blank = keep current)",  "", sizeof(Settings::mqttPass) - 1, "type='password'");
static WiFiManagerParameter pTopic("topic",  "MQTT base topic",                       "", sizeof(Settings::baseTopic) - 1);
static WiFiManagerParameter pAdmin("admin",  "OTA update password (blank = keep current)", "", sizeof(Settings::adminPass) - 1, "type='password'");

// ────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ────────────────────────────────────────────────────────────────────────────
static CmdResult submitCommand(Command::Type type, const String& slot);
static void      executeCommand(const Command& c);
static String    buildStatusJson();
static String    buildSlotsJson();
static void      publishStatus();
static void      startServices();
static void      setupOTA();
static void      configureWatchdog();
static void      checkResetButton();
static void      checkWifiHealth();
static void      factoryReset();

// ────────────────────────────────────────────────────────────────────────────
// setup()
// ────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[FanSpitter] booting, firmware %s\n", FW_VERSION);
  Serial.printf("[FanSpitter] reset reason: %d\n", (int)esp_reset_reason());

  cmdQueue = xQueueCreate(4, sizeof(Command));

  // NVS signal storage (needed by the web UI even if the radio is dead)
  signalStore.begin();

  // CC1101 on VSPI. If it doesn't answer, keep running without RF so the
  // device stays reachable for diagnosis and OTA instead of halting.
  SPI.begin();
  for (int attempt = 1; attempt <= 3 && !rfOk; attempt++) {
    rfOk = cc1101.begin();
    if (!rfOk) {
      Serial.printf("[App] CC1101 not responding (attempt %d/3)\n", attempt);
      delay(500);
    }
  }
  if (rfOk) {
    rfLearn.begin();
    rfLearn.onLearnDone([](bool ok, const char* reason) {
      const String slot = pendingLearnSlot;
      if (ok) {
        uint16_t count = rfLearn.getConfirmedPulses(txBuf, RF_MAX_PULSES);
        if (count > 0 && signalStore.save(slot, txBuf, count)) {
          Serial.printf("[App] Learned \"%s\" (%u pulses)\n", slot.c_str(), count);
          if (mqtt) mqtt->publishLearned(slot, true, count, nullptr);
        } else {
          Serial.printf("[App] Learned \"%s\" but saving failed\n", slot.c_str());
          if (mqtt) mqtt->publishLearned(slot, false, 0, "store_failed");
        }
      } else {
        Serial.printf("[App] Learn \"%s\" failed: %s\n", slot.c_str(), reason);
        if (mqtt) mqtt->publishLearned(slot, false, 0, reason);
      }
      appState = AppState::IDLE;
      statusDirty = true;
    });
  } else {
    Serial.println("[App] ERROR: running without RF — check CC1101 wiring/power");
  }

  // Settings + captive portal
  loadSettings();
  pHost.setValue(settings.mqttHost, sizeof(settings.mqttHost) - 1);
  pPort.setValue(settings.mqttPort, sizeof(settings.mqttPort) - 1);
  pUser.setValue(settings.mqttUser, sizeof(settings.mqttUser) - 1);
  pTopic.setValue(settings.baseTopic, sizeof(settings.baseTopic) - 1);
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.addParameter(&pUser);
  wm.addParameter(&pPass);
  wm.addParameter(&pTopic);
  wm.addParameter(&pAdmin);

  wm.setSaveParamsCallback([]() {
    strlcpy(settings.mqttHost,  pHost.getValue(),  sizeof(settings.mqttHost));
    strlcpy(settings.mqttPort,  pPort.getValue(),  sizeof(settings.mqttPort));
    strlcpy(settings.mqttUser,  pUser.getValue(),  sizeof(settings.mqttUser));
    strlcpy(settings.baseTopic, pTopic.getValue(), sizeof(settings.baseTopic));
    if (pPass.getValue()[0])  strlcpy(settings.mqttPass,  pPass.getValue(),  sizeof(settings.mqttPass));
    if (pAdmin.getValue()[0]) strlcpy(settings.adminPass, pAdmin.getValue(), sizeof(settings.adminPass));
    if (settings.baseTopic[0] == '\0') strlcpy(settings.baseTopic, "fanspitter", sizeof(settings.baseTopic));
    saveSettings();
    Serial.printf("[App] Settings saved (MQTT %s:%s, topic %s)\n",
                  settings.mqttHost[0] ? settings.mqttHost : "<disabled>",
                  settings.mqttPort, settings.baseTopic);
    // Services read settings once at start; restart so new values apply.
    if (servicesStarted) restartPending = true;
  });

  // If nobody configures the portal in time and WiFi is still down, reboot
  // and try the saved network again rather than sitting offline forever.
  wm.setConfigPortalTimeoutCallback([]() {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[App] Setup portal timed out without WiFi — rebooting");
      restartPending = true;
    }
  });

  WiFi.mode(WIFI_STA);  // init the WiFi stack so saved credentials can be read
  wm.setHostname(DEVICE_HOSTNAME);
  wm.setConfigPortalBlocking(false);
  // Unconfigured: keep the portal up indefinitely. Configured but the network
  // is unreachable (e.g. router rebooting): portal for 3 min, then reboot and
  // retry the saved network.
  wm.setConfigPortalTimeout(wm.getWiFiIsSaved() ? 180 : 0);
  wm.setConnectTimeout(20);
  if (wm.autoConnect(SETUP_AP_NAME)) {
    Serial.println("[App] WiFi connected");
  } else {
    Serial.printf("[App] Setup portal running: join WiFi \"%s\"\n", SETUP_AP_NAME);
  }

#if PIN_RESET_BTN >= 0
  pinMode(PIN_RESET_BTN, INPUT_PULLUP);
#endif

  configureWatchdog();
  Serial.println("[App] setup done");
}

// ────────────────────────────────────────────────────────────────────────────
// loop()
// ────────────────────────────────────────────────────────────────────────────
void loop() {
  wm.process();

  if (!servicesStarted && WiFi.status() == WL_CONNECTED) {
    startServices();
    servicesStarted = true;
  }

  if (servicesStarted) {
    if (otaEnabled) ArduinoOTA.handle();
    if (mqtt) {
      mqtt->loop();
      bool up = mqtt->isConnected();
      if (up != mqttUp.load()) { mqttUp = up; statusDirty = true; }
    }
  }

  // Execute queued commands (web + MQTT)
  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) executeCommand(c);

  if (rfOk) rfLearn.process();

  // Status → MQTT on change, plus a periodic heartbeat for RSSI/uptime
  static uint32_t lastStatusMs = 0;
  if (mqttUp && (statusDirty || millis() - lastStatusMs > STATUS_HEARTBEAT_MS)) {
    publishStatus();
    lastStatusMs = millis();
  }

  checkResetButton();
  checkWifiHealth();

  if (restartPending) {
    Serial.println("[App] Restarting...");
    Serial.flush();
    delay(500);
    ESP.restart();
  }
}

// ────────────────────────────────────────────────────────────────────────────
// startServices() — called once when WiFi first connects
// ────────────────────────────────────────────────────────────────────────────
static void startServices() {
  Serial.printf("[App] IP %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

  // Mains-powered device: disable modem sleep so commands arrive promptly.
  WiFi.setSleep(false);

  // The portal and our web server both want port 80.
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();

  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[App] mDNS: http://%s.local/\n", DEVICE_HOSTNAME);
  } else {
    Serial.println("[App] WARN: mDNS failed to start");
  }

  setupOTA();  // only active with a password configured

  webUI.begin(
    [](const String& s) { return submitCommand(Command::Type::LEARN,  s); },
    [](const String& s) { return submitCommand(Command::Type::SEND,   s); },
    [](const String& s) { return submitCommand(Command::Type::DELETE, s); },
    []() { return buildStatusJson(); },
    []() { return buildSlotsJson(); });
  server.begin();
  Serial.println("[App] HTTP server started");

  MqttConfig mc;
  mc.host      = settings.mqttHost;
  mc.port      = (uint16_t)atoi(settings.mqttPort);
  mc.user      = settings.mqttUser;
  mc.pass      = settings.mqttPass;
  mc.baseTopic = settings.baseTopic;
  if (mc.port == 0) mc.port = 1883;

  if (mc.host.length() == 0) {
    Serial.println("[App] MQTT disabled (no broker configured)");
    return;
  }

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mqtt = new MQTTHandler(mc);
  mqtt->begin("fanspitter-" + mac,
    [](const String& s) {
      CmdResult r = submitCommand(Command::Type::LEARN, s);
      if (r != CmdResult::OK) {
        Serial.printf("[App] MQTT learn \"%s\" rejected: %s\n", s.c_str(), cmdResultStr(r));
        if (mqtt) mqtt->publishLearned(s, false, 0, cmdResultStr(r));
      }
    },
    [](const String& s) {
      CmdResult r = submitCommand(Command::Type::SEND, s);
      if (r != CmdResult::OK) {
        Serial.printf("[App] MQTT cmd \"%s\" rejected: %s\n", s.c_str(), cmdResultStr(r));
      }
    },
    []() { statusDirty = true; });
}

// ────────────────────────────────────────────────────────────────────────────
// Commands. submitCommand() may run on any task; executeCommand() runs on
// the loop task only.
// ────────────────────────────────────────────────────────────────────────────
static CmdResult submitCommand(Command::Type type, const String& slot) {
  if (!isValidSlotName(slot.c_str(), slot.length())) return CmdResult::INVALID_NAME;

  switch (type) {
    case Command::Type::LEARN:
      if (!rfOk) return CmdResult::RF_UNAVAILABLE;
      if (appState != AppState::IDLE) return CmdResult::BUSY;
      if (!signalStore.exists(slot) && signalStore.isFull()) return CmdResult::STORE_FULL;
      break;
    case Command::Type::SEND:
      if (!rfOk) return CmdResult::RF_UNAVAILABLE;
      if (appState == AppState::LEARNING) return CmdResult::BUSY;
      if (!signalStore.exists(slot)) return CmdResult::NOT_FOUND;
      break;
    case Command::Type::DELETE:
      if (!signalStore.exists(slot)) return CmdResult::NOT_FOUND;
      break;
  }

  Command c;
  c.type = type;
  strlcpy(c.slot, slot.c_str(), sizeof(c.slot));
  if (xQueueSend(cmdQueue, &c, 0) != pdTRUE) return CmdResult::QUEUE_FULL;
  return CmdResult::OK;
}

static void executeCommand(const Command& c) {
  const String slot(c.slot);

  switch (c.type) {
    case Command::Type::LEARN: {
      if (appState != AppState::IDLE) {
        Serial.printf("[App] Learn \"%s\" dropped: busy\n", c.slot);
        return;
      }
      pendingLearnSlot = slot;
      portENTER_CRITICAL(&learnSlotMux);
      strlcpy(learnSlotName, c.slot, sizeof(learnSlotName));
      portEXIT_CRITICAL(&learnSlotMux);
      if (rfLearn.startLearn()) {
        learnStartMs = millis();
        appState = AppState::LEARNING;
        statusDirty = true;
      }
      break;
    }

    case Command::Type::SEND: {
      if (appState != AppState::IDLE) {
        Serial.printf("[App] Send \"%s\" dropped: busy\n", c.slot);
        return;
      }
      uint16_t count = signalStore.load(slot, txBuf, RF_MAX_PULSES);
      if (count == 0) return;
      appState = AppState::TRANSMITTING;
      if (mqttUp) publishStatus();
      if (!rfLearn.transmit(txBuf, count)) {
        Serial.printf("[App] Send \"%s\" failed\n", c.slot);
      }
      appState = AppState::IDLE;
      statusDirty = true;
      break;
    }

    case Command::Type::DELETE: {
      signalStore.remove(slot);
      statusDirty = true;
      break;
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// Status — one JSON document for /api/status and <base>/status.
// Safe to call from any task.
// ────────────────────────────────────────────────────────────────────────────
static String buildStatusJson() {
  JsonDocument doc;
  const AppState st = appState.load();

  switch (st) {
    case AppState::LEARNING:     doc["state"] = "learning";     break;
    case AppState::TRANSMITTING: doc["state"] = "transmitting"; break;
    default:                     doc["state"] = "idle";         break;
  }

  int learnSecs = 0;
  if (st == AppState::LEARNING) {
    long remaining = ((long)RF_LEARN_TIMEOUT_MS - (long)(millis() - learnStartMs.load())) / 1000L;
    learnSecs = remaining > 0 ? (int)remaining : 0;
  }
  doc["learnSecs"] = learnSecs;

  char learnSlot[SLOT_NAME_MAX + 1] = "";
  if (st == AppState::LEARNING) {
    portENTER_CRITICAL(&learnSlotMux);
    strlcpy(learnSlot, learnSlotName, sizeof(learnSlot));
    portEXIT_CRITICAL(&learnSlotMux);
  }
  doc["learnSlot"] = learnSlot;  // char[] is copied into the document

  String names[SignalStore::MAX_SLOTS];
  signalStore.listNames(names);
  JsonArray arr = doc["slots"].to<JsonArray>();
  for (const String& n : names) {
    if (n.length() > 0) arr.add(n);
  }
  doc["slotCount"] = arr.size();
  doc["maxSlots"]  = SignalStore::MAX_SLOTS;

  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  doc["ip"]      = wifiUp ? WiFi.localIP().toString() : String("disconnected");
  doc["rssi"]    = wifiUp ? (int)WiFi.RSSI() : 0;
  doc["mqtt"]    = mqttUp.load();
  doc["rf"]      = rfOk;
  doc["ota"]     = otaEnabled;
  doc["uptime"]  = (uint32_t)(millis() / 1000);
  doc["heap"]    = ESP.getFreeHeap();
  doc["version"] = FW_VERSION;

  String json;
  serializeJson(doc, json);
  return json;
}

static String buildSlotsJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  String names[SignalStore::MAX_SLOTS];
  signalStore.listNames(names);
  for (const String& n : names) {
    if (n.length() > 0) arr.add(n);
  }
  String json;
  serializeJson(doc, json);
  return json;
}

static void publishStatus() {
  if (!mqtt) return;
  statusDirty = false;
  if (!mqtt->publishStatus(buildStatusJson())) statusDirty = true;
}

// ────────────────────────────────────────────────────────────────────────────
// OTA — `pio run -e ota -t upload` (see README). Requires a password.
// ────────────────────────────────────────────────────────────────────────────
static void setupOTA() {
  if (settings.adminPass[0] == '\0') {
    Serial.println("[OTA] Disabled: no OTA password set in the setup portal");
    return;
  }
  ArduinoOTA.setHostname(DEVICE_HOSTNAME);
  ArduinoOTA.setPassword(settings.adminPass);
  ArduinoOTA.setMdnsEnabled(false);       // mDNS is already running
  MDNS.enableArduino(3232, /*auth=*/true);
  ArduinoOTA.onStart([]() {
    // The update loop blocks for the whole transfer without feeding the
    // loop watchdog; suspend it until we reboot (or the update fails).
    disableLoopWDT();
    if (rfOk) rfLearn.cancelLearn();
    appState = AppState::IDLE;
    Serial.println("[OTA] Update started");
  });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Update complete, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Error %u\n", (unsigned)e);
    enableLoopWDT();
  });
  ArduinoOTA.begin();
  otaEnabled = true;
  Serial.printf("[OTA] Ready at %s.local:3232\n", DEVICE_HOSTNAME);
}

// ────────────────────────────────────────────────────────────────────────────
// Health checks
// ────────────────────────────────────────────────────────────────────────────
static void configureWatchdog() {
  // Lengthen the task watchdog (default 5 s) so a slow-but-bounded MQTT
  // connect can't trip it, then subscribe the loop task. A genuinely hung
  // loop now reboots the device instead of leaving it dead on the ceiling.
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = LOOP_WDT_TIMEOUT_MS;
  cfg.idle_core_mask = (1 << 0);   // same idle-task coverage as the default
  cfg.trigger_panic  = true;
  esp_task_wdt_reconfigure(&cfg);
  enableLoopWDT();
}

// Factory reset fires when the button is RELEASED after being held between
// FACTORY_RESET_HOLD_MS and 30 s. Longer holds are ignored: GPIO0 can be held
// low indefinitely by a USB-serial adapter's DTR line, and that must never
// wipe the device.
static void checkResetButton() {
#if PIN_RESET_BTN >= 0
  static constexpr uint32_t MAX_HOLD_MS = 30000;
  static uint32_t downSince = 0;
  static bool     announced = false;
  const bool pressed = digitalRead(PIN_RESET_BTN) == LOW;

  if (pressed) {
    if (downSince == 0) downSince = millis() | 1;
    uint32_t held = millis() - downSince;
    if (!announced && held >= FACTORY_RESET_HOLD_MS && held <= MAX_HOLD_MS) {
      Serial.println("[App] Release button now to factory reset");
      announced = true;
    }
    return;
  }

  if (downSince != 0) {
    uint32_t held = millis() - downSince;
    if (held >= FACTORY_RESET_HOLD_MS && held <= MAX_HOLD_MS) factoryReset();
  }
  downSince = 0;
  announced = false;
#endif
}

static void factoryReset() {
  Serial.println("[App] Factory reset: clearing WiFi + settings (signals kept)");
  Preferences p;
  if (p.begin("fs_conf", false)) {
    p.clear();
    p.end();
  }
  wm.resetSettings();
  restartPending = true;
}

static void checkWifiHealth() {
  static uint32_t downSince = 0;
  if (WiFi.status() == WL_CONNECTED || wm.getConfigPortalActive()) {
    downSince = 0;
    return;
  }
  if (downSince == 0) {
    downSince = millis() | 1;
  } else if (millis() - downSince > WIFI_DOWN_REBOOT_MS) {
    Serial.println("[App] WiFi down too long — rebooting");
    restartPending = true;
  }
}
