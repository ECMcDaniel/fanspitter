#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include "signal_store.h"

// ────────────────────────────────────────────────────────────────────────────
// WebUI — embedded single-page app on ESPAsyncWebServer
//
// Routes:
//   GET  /              — full HTML page (PROGMEM)
//   GET  /api/status    — JSON {"state","slots","ip","mqtt","learnSecs"}
//   GET  /api/slots     — JSON array of slot name strings
//   POST /api/learn     — body {"slot":"name"} — enter learn mode
//   POST /api/send      — body {"slot":"name"} — replay signal
//   DELETE /api/slot    — body {"slot":"name"} — delete slot
// ────────────────────────────────────────────────────────────────────────────
class WebUI {
public:
  using SlotCb        = std::function<void(const String& slot)>;
  using DeleteCb      = std::function<void(const String& slot)>;
  using StatusJson    = std::function<String()>;  // returns JSON string for /api/status

  WebUI(AsyncWebServer& server, SignalStore& store);

  // Register all routes. Call once after WiFi connects.
  void begin(SlotCb   onLearn,
             SlotCb   onSend,
             DeleteCb onDelete,
             StatusJson statusProvider);

private:
  AsyncWebServer& _server;
  SignalStore&    _store;
  SlotCb          _onLearn;
  SlotCb          _onSend;
  DeleteCb        _onDelete;
  StatusJson      _statusProvider;

  void registerRoutes();
};
