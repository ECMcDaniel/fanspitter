#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include "commands.h"

// ────────────────────────────────────────────────────────────────────────────
// WebUI — embedded single-page app on ESPAsyncWebServer
//
// Routes:
//   GET    /            — HTML page
//   GET    /api/status  — JSON status (same document published to MQTT)
//   GET    /api/slots   — JSON array of slot names
//   POST   /api/learn   — {"slot":"name"} — enter learn mode
//   POST   /api/send    — {"slot":"name"} — replay signal
//   DELETE /api/slot    — {"slot":"name"} — delete slot
//
// Handlers run in the AsyncTCP task. They never touch the radio: commands are
// handed to the loop task through the callbacks, which validate and enqueue.
// ────────────────────────────────────────────────────────────────────────────
class WebUI {
public:
  using CommandCb  = std::function<CmdResult(const String& slot)>;
  using JsonCb     = std::function<String()>;

  explicit WebUI(AsyncWebServer& server) : _server(server) {}

  // Register all routes. Call once before server.begin().
  void begin(CommandCb onLearn, CommandCb onSend, CommandCb onDelete,
             JsonCb statusJson, JsonCb slotsJson);

private:
  AsyncWebServer& _server;
  CommandCb _onLearn, _onSend, _onDelete;
  JsonCb    _statusJson, _slotsJson;

  void addCommandRoute(const char* path, WebRequestMethodComposite method, CommandCb* cb);
};
