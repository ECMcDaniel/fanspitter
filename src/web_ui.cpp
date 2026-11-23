#include "web_ui.h"
#include <ArduinoJson.h>

// ────────────────────────────────────────────────────────────────────────────
// Embedded page — lives in flash and is streamed without copying to RAM.
// Slot names are validated server-side to [A-Za-z0-9_-] and are only ever
// inserted into the DOM via textContent / dataset, never as HTML or JS.
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
.meta{font-size:.8rem;color:var(--sub);margin-bottom:1.2rem;word-break:break-word}
.chip{display:inline-block;padding:.2em .7em;border-radius:99px;font-size:.75rem;font-weight:700;background:#333;color:var(--txt);margin-left:.5rem;vertical-align:middle}
.chip.idle{background:#1e4d2b;color:var(--ok)}
.chip.learning{background:#4d3a1e;color:#fa0}
.chip.transmitting{background:#1e2e4d;color:#6af}
.chip.offline,.chip.error{background:#4d1e1e;color:#f66}
section{background:var(--card);border-radius:8px;padding:1rem;margin-bottom:1rem}
section h2{font-size:1rem;margin-bottom:.75rem;color:var(--sub)}
.row{display:flex;gap:.5rem;align-items:center}
.hint{font-size:.7rem;color:var(--sub);margin-top:.4rem}
input[type=text]{flex:1;min-width:0;padding:.5rem .75rem;border-radius:6px;border:1px solid #333;background:#111;color:var(--txt);font-size:.9rem}
input[type=text]:focus{outline:none;border-color:var(--accent)}
button{padding:.5rem 1rem;border-radius:6px;border:none;cursor:pointer;font-size:.85rem;font-weight:600;transition:.15s}
button:hover{opacity:.85}
button:disabled{opacity:.4;cursor:not-allowed}
.btn-learn{background:var(--accent);color:#fff}
.btn-send{background:#1e4d2b;color:var(--ok)}
.btn-del{background:#4d1e1e;color:#f66}
#progress-wrap{margin-top:.75rem;display:none}
progress{width:100%;height:8px;border-radius:4px;appearance:none}
progress::-webkit-progress-bar{background:#333;border-radius:4px}
progress::-webkit-progress-value{background:#fa0;border-radius:4px}
#progress-label{font-size:.75rem;color:#fa0;margin-top:.25rem}
#lnow{font-size:1.15rem;margin-bottom:.5rem}
#lslot{color:#fa0;word-break:break-all}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:.75rem;color:var(--sub);padding:.3rem .5rem;border-bottom:1px solid #333}
td{padding:.5rem;font-size:.85rem;border-bottom:1px solid #222;word-break:break-all}
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
<input type="text" id="sname" placeholder="slot name (e.g. fan_high)" maxlength="32" pattern="[A-Za-z0-9_\-]{1,32}" autocomplete="off">
<button class="btn-learn" id="learn">Start&nbsp;Learn</button>
</div>
<div class="hint">Letters, digits, _ and - only. Re-using a name overwrites it.</div>
<div id="progress-wrap">
<div id="lnow">Now learning: <b id="lslot"></b></div>
<progress id="prog" max="30" value="30"></progress>
<div id="progress-label">Hold that remote button for a second... <span id="secs"></span>s</div>
</div>
</section>
<section>
<h2>Saved Signals <span id="count"></span></h2>
<table><thead><tr><th>Name</th><th>Send</th><th>Delete</th></tr></thead>
<tbody id="slots"></tbody></table>
</section>
<div class="toast" id="toast"></div>
<script>
const $=id=>document.getElementById(id);
let lastSlots='';
function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(j=>{
    const st=j.rf===false?'error':j.state;
    $('chip').textContent=j.rf===false?'RF ERROR':j.state.toUpperCase();
    $('chip').className='chip '+st;
    $('meta').textContent='IP: '+(j.ip||'?')+'  |  MQTT: '+(j.mqtt?'✓':'✗')+
      '  |  RSSI: '+(j.rssi||'?')+' dBm  |  fw '+(j.version||'?');
    $('count').textContent='('+(j.slotCount||0)+'/'+(j.maxSlots||'?')+')';
    renderSlots(j.slots||[]);
    const learning=j.state==='learning';
    $('learn').disabled=learning||j.rf===false;
    $('progress-wrap').style.display=learning?'block':'none';
    if(learning){$('prog').value=j.learnSecs;$('secs').textContent=j.learnSecs;$('lslot').textContent=j.learnSlot||'?';}
  }).catch(()=>{$('chip').textContent='OFFLINE';$('chip').className='chip offline';});
}
function renderSlots(slots){
  const key=JSON.stringify(slots);
  if(key===lastSlots)return;
  lastSlots=key;
  const tb=$('slots');
  tb.replaceChildren();
  if(!slots.length){
    const tr=tb.insertRow(),td=tr.insertCell();
    td.colSpan=3;td.className='empty';td.textContent='No signals saved yet.';
    return;
  }
  for(const n of slots){
    const tr=tb.insertRow();
    tr.insertCell().textContent=n;
    for(const [cls,label,act] of [['btn-send','Send','send'],['btn-del','Delete','del']]){
      const b=document.createElement('button');
      b.className=cls;b.textContent=label;b.dataset.act=act;b.dataset.name=n;
      tr.insertCell().appendChild(b);
    }
  }
}
function toast(msg,ok=true){
  const t=$('toast');
  t.textContent=msg;t.style.background=ok?'#1e4d2b':'#4d1e1e';
  t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2500);
}
async function api(method,path,slot){
  try{
    const r=await fetch(path,{method,headers:{'Content-Type':'application/json'},body:JSON.stringify({slot})});
    const j=await r.json().catch(()=>({}));
    return {ok:r.ok,error:j.error||('HTTP '+r.status)};
  }catch(e){return {ok:false,error:'device unreachable'};}
}
$('learn').onclick=async()=>{
  const name=$('sname').value.trim();
  if(!/^[A-Za-z0-9_-]{1,32}$/.test(name)){toast('Use 1-32 letters, digits, _ or -',false);return;}
  const r=await api('POST','/api/learn',name);
  toast(r.ok?'Learning: hold the remote button for a second':r.error,r.ok);
  fetchStatus();
};
$('sname').addEventListener('keydown',e=>{if(e.key==='Enter')$('learn').click();});
$('slots').addEventListener('click',async e=>{
  const b=e.target.closest('button');
  if(!b)return;
  const name=b.dataset.name;
  if(b.dataset.act==='send'){
    const r=await api('POST','/api/send',name);
    toast(r.ok?'Sent: '+name:r.error,r.ok);
  }else{
    if(!confirm('Delete "'+name+'"?'))return;
    const r=await api('DELETE','/api/slot',name);
    toast(r.ok?'Deleted: '+name:r.error,r.ok);
    fetchStatus();
  }
});
fetchStatus();
setInterval(fetchStatus,1000);
</script></body></html>
)rawliteral";

static int httpCodeFor(CmdResult r) {
  switch (r) {
    case CmdResult::OK:             return 200;
    case CmdResult::INVALID_NAME:   return 400;
    case CmdResult::NOT_FOUND:      return 404;
    case CmdResult::BUSY:           return 409;
    case CmdResult::STORE_FULL:     return 507;
    case CmdResult::QUEUE_FULL:     return 503;
    case CmdResult::RF_UNAVAILABLE: return 503;
  }
  return 500;
}

static void sendError(AsyncWebServerRequest* req, int code, const char* msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String body;
  serializeJson(doc, body);
  req->send(code, "application/json", body);
}

void WebUI::begin(CommandCb onLearn, CommandCb onSend, CommandCb onDelete,
                  JsonCb statusJson, JsonCb slotsJson) {
  _onLearn    = onLearn;
  _onSend     = onSend;
  _onDelete   = onDelete;
  _statusJson = statusJson;
  _slotsJson  = slotsJson;

  _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", (const uint8_t*)HTML_PAGE, strlen_P(HTML_PAGE));
  });

  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res =
      req->beginResponse(200, "application/json", _statusJson ? _statusJson() : String("{}"));
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  _server.on("/api/slots", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->send(200, "application/json", _slotsJson ? _slotsJson() : String("[]"));
  });

  addCommandRoute("/api/learn", HTTP_POST,   &_onLearn);
  addCommandRoute("/api/send",  HTTP_POST,   &_onSend);
  addCommandRoute("/api/slot",  HTTP_DELETE, &_onDelete);

  _server.onNotFound([](AsyncWebServerRequest* req) {
    sendError(req, 404, "not found");
  });
}

void WebUI::addCommandRoute(const char* path, WebRequestMethodComposite method, CommandCb* cb) {
  auto* h = new AsyncCallbackJsonWebHandler(path,
    [cb](AsyncWebServerRequest* req, JsonVariant& body) {
      if (!body.is<JsonObject>() || !body["slot"].is<const char*>()) {
        sendError(req, 400, "expected JSON body {\"slot\":\"name\"}");
        return;
      }
      String slot = body["slot"].as<String>();
      slot.trim();
      CmdResult r = (*cb) ? (*cb)(slot) : CmdResult::RF_UNAVAILABLE;
      if (r == CmdResult::OK) {
        req->send(200, "application/json", "{\"ok\":true}");
      } else {
        sendError(req, httpCodeFor(r), cmdResultStr(r));
      }
    });
  h->setMethod(method);
  h->setMaxContentLength(256);
  _server.addHandler(h);
}
