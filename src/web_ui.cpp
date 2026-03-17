#include "web_ui.h"
#include <ArduinoJson.h>

// ────────────────────────────────────────────────────────────────────────────
// Embedded page — stored in flash (PROGMEM)
// ────────────────────────────────────────────────────────────────────────────
static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FanSpitter</title>
<style>
:root{--bg:#1a1a2e;--card:#16213e;--accent:#e94560;--ok:#0f9;--txt:#eee;--sub:#aaa}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--txt);padding:1rem;max-width:600px;margin:0 auto}
h1{font-size:1.5rem;color:var(--accent);margin-bottom:.25rem}
.meta{font-size:.8rem;color:var(--sub);margin-bottom:1.2rem}
.chip{display:inline-block;padding:.2em .7em;border-radius:99px;font-size:.75rem;font-weight:700;background:#333;color:var(--txt);margin-left:.5rem;vertical-align:middle}
.chip.idle{background:#1e4d2b;color:var(--ok)}
.chip.learning{background:#4d3a1e;color:#fa0}
.chip.transmitting{background:#1e2e4d;color:#6af}
.chip.offline{background:#4d1e1e;color:#f66}
section{background:var(--card);border-radius:8px;padding:1rem;margin-bottom:1rem}
section h2{font-size:1rem;margin-bottom:.75rem;color:var(--sub)}
.row{display:flex;gap:.5rem;align-items:center}
input[type=text]{flex:1;padding:.5rem .75rem;border-radius:6px;border:1px solid #333;background:#111;color:var(--txt);font-size:.9rem}
input[type=text]:focus{outline:none;border-color:var(--accent)}
button{padding:.5rem 1rem;border-radius:6px;border:none;cursor:pointer;font-size:.85rem;font-weight:600;transition:.15s}
.btn-learn{background:var(--accent);color:#fff}
.btn-learn:hover{opacity:.85}
.btn-send{background:#1e4d2b;color:var(--ok)}
.btn-send:hover{opacity:.85}
.btn-del{background:#4d1e1e;color:#f66}
.btn-del:hover{opacity:.85}
#progress-wrap{margin-top:.75rem;display:none}
progress{width:100%;height:8px;border-radius:4px;appearance:none}
progress::-webkit-progress-bar{background:#333;border-radius:4px}
progress::-webkit-progress-value{background:#fa0;border-radius:4px}
#progress-label{font-size:.75rem;color:#fa0;margin-top:.25rem}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:.75rem;color:var(--sub);padding:.3rem .5rem;border-bottom:1px solid #333}
td{padding:.5rem;font-size:.85rem;border-bottom:1px solid #222}
tr:last-child td{border-bottom:none}
.empty{text-align:center;color:var(--sub);padding:1.5rem}
.toast{position:fixed;bottom:1rem;left:50%;transform:translateX(-50%);background:#222;color:#fff;padding:.5rem 1.2rem;border-radius:6px;font-size:.85rem;opacity:0;transition:.3s;pointer-events:none;z-index:99}
.toast.show{opacity:1}
</style></head><body>
<h1>FanSpitter <span id="chip" class="chip">...</span></h1>
<div class="meta" id="meta">connecting...</div>
<section>
<h2>Learn New Signal</h2>
<div class="row">
<input type="text" id="sname" placeholder="slot name (e.g. fan_high)" maxlength="14">
<button class="btn-learn" onclick="startLearn()">Start&nbsp;Learn</button>
</div>
<div id="progress-wrap">
<progress id="prog" max="30" value="30"></progress>
<div id="progress-label">Waiting for remote... <span id="secs"></span>s</div>
</div>
</section>
<section>
<h2>Saved Signals</h2>
<table><thead><tr><th>Name</th><th>Send</th><th>Delete</th></tr></thead>
<tbody id="slots"></tbody></table>
</section>
<div class="toast" id="toast"></div>
<script>
let lastState='';
function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(j=>{
    lastState=j.state;
    const chip=document.getElementById('chip');
    chip.textContent=j.state.toUpperCase();
    chip.className='chip '+j.state;
    document.getElementById('meta').textContent=
      'IP: '+(j.ip||'?')+'  |  MQTT: '+(j.mqtt?'✓':'✗')+'  |  RSSI: '+(j.rssi||'?')+'dBm';
    renderSlots(j.slots||[]);
    if(j.state==='learning'&&j.learnSecs>0){
      document.getElementById('progress-wrap').style.display='block';
      document.getElementById('prog').value=j.learnSecs;
      document.getElementById('secs').textContent=j.learnSecs;
    } else {
      document.getElementById('progress-wrap').style.display='none';
    }
  }).catch(()=>{});
}
function renderSlots(slots){
  const tb=document.getElementById('slots');
  if(!slots.length){tb.innerHTML='<tr><td colspan="3" class="empty">No signals saved yet.</td></tr>';return;}
  tb.innerHTML=slots.map(n=>`<tr>
<td>${esc(n)}</td>
<td><button class="btn-send" onclick="sendSlot('${esc(n)}')">Send</button></td>
<td><button class="btn-del" onclick="delSlot('${esc(n)}')">Delete</button></td>
</tr>`).join('');
}
function esc(s){return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function toast(msg,ok=true){
  const t=document.getElementById('toast');
  t.textContent=msg;t.style.background=ok?'#1e4d2b':'#4d1e1e';
  t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);
}
async function startLearn(){
  const name=document.getElementById('sname').value.trim();
  if(!name){toast('Enter a slot name',false);return;}
  const r=await fetch('/api/learn',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:name})});
  if(r.ok)toast('Learning started — press remote twice');
  else{const j=await r.json();toast(j.error||'Error',false);}
  fetchStatus();
}
async function sendSlot(name){
  const r=await fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:name})});
  if(r.ok)toast('Signal sent: '+name);
  else{const j=await r.json();toast(j.error||'Error',false);}
}
async function delSlot(name){
  if(!confirm('Delete "'+name+'"?'))return;
  const r=await fetch('/api/slot',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:name})});
  if(r.ok)toast('Deleted: '+name);
  else{const j=await r.json();toast(j.error||'Error',false);}
  fetchStatus();
}
fetchStatus();
setInterval(fetchStatus,2000);
</script></body></html>
)rawliteral";

// ────────────────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────────────────
WebUI::WebUI(AsyncWebServer& server, SignalStore& store)
  : _server(server), _store(store) {}

// ────────────────────────────────────────────────────────────────────────────
// begin()
// ────────────────────────────────────────────────────────────────────────────
void WebUI::begin(SlotCb    onLearn,
                  SlotCb    onSend,
                  DeleteCb  onDelete,
                  StatusJson statusProvider) {
  _onLearn        = onLearn;
  _onSend         = onSend;
  _onDelete       = onDelete;
  _statusProvider = statusProvider;
  registerRoutes();
}

// ────────────────────────────────────────────────────────────────────────────
// registerRoutes()
// ────────────────────────────────────────────────────────────────────────────
void WebUI::registerRoutes() {

  // ── GET / ────────────────────────────────────────────────────────────────
  _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML_PAGE);
  });

  // ── GET /api/status ──────────────────────────────────────────────────────
  _server.on("/api/status", HTTP_GET,
    [this](AsyncWebServerRequest* req) {
      String json = _statusProvider ? _statusProvider() : "{}";
      req->send(200, "application/json", json);
    });

  // ── GET /api/slots ───────────────────────────────────────────────────────
  _server.on("/api/slots", HTTP_GET,
    [this](AsyncWebServerRequest* req) {
      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      String names[SignalStore::MAX_SLOTS];
      _store.listNames(names);
      for (uint8_t i = 0; i < SignalStore::MAX_SLOTS; i++) {
        if (names[i].length() > 0) arr.add(names[i]);
      }
      String json;
      serializeJson(doc, json);
      req->send(200, "application/json", json);
    });

  // ── Reusable send helper ─────────────────────────────────────────────────
  auto sendJson = [](AsyncWebServerRequest* req, int code, const char* msg) {
    String body = "{\"error\":\"";
    body += msg;
    body += "\"}";
    req->send(code, "application/json", body);
  };

  // ── POST /api/learn ──────────────────────────────────────────────────────
  auto* learnH = new AsyncCallbackJsonWebHandler("/api/learn",
    [this, sendJson](AsyncWebServerRequest* req, JsonVariant& body) {
      if (!body.is<JsonObject>()) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String slot = body["slot"].as<String>();
      slot.trim();
      if (slot.length() == 0) { sendJson(req, 400, "slot required"); return; }
      if (_onLearn) _onLearn(slot);
      req->send(200, "application/json", "{\"ok\":true}");
    });
  learnH->setMethod(HTTP_POST);
  _server.addHandler(learnH);

  // ── POST /api/send ───────────────────────────────────────────────────────
  auto* sendH = new AsyncCallbackJsonWebHandler("/api/send",
    [this, sendJson](AsyncWebServerRequest* req, JsonVariant& body) {
      if (!body.is<JsonObject>()) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String slot = body["slot"].as<String>();
      slot.trim();
      if (slot.length() == 0) { sendJson(req, 400, "slot required"); return; }
      if (!_store.exists(slot)) { sendJson(req, 404, "slot not found"); return; }
      if (_onSend) _onSend(slot);
      req->send(200, "application/json", "{\"ok\":true}");
    });
  sendH->setMethod(HTTP_POST);
  _server.addHandler(sendH);

  // ── DELETE /api/slot ─────────────────────────────────────────────────────
  auto* delH = new AsyncCallbackJsonWebHandler("/api/slot",
    [this, sendJson](AsyncWebServerRequest* req, JsonVariant& body) {
      if (!body.is<JsonObject>()) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String slot = body["slot"].as<String>();
      slot.trim();
      if (slot.length() == 0) { sendJson(req, 400, "slot required"); return; }
      if (!_store.remove(slot)) { sendJson(req, 404, "slot not found"); return; }
      if (_onDelete) _onDelete(slot);
      req->send(200, "application/json", "{\"ok\":true}");
    });
  delH->setMethod(HTTP_DELETE);
  _server.addHandler(delH);

  // ── 404 ──────────────────────────────────────────────────────────────────
  _server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
  });
}
