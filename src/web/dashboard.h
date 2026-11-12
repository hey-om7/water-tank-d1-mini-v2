#pragma once
// ---------------------------------------------------------------------------
//  dashboard.h - web UI + JSON API on http://watertank.local (port 80).
//
//    GET  /              dashboard (tank %, polls /api/status) + settings
//    GET  /api/status    {"level","rawLevel","distance","valid","filling",...}
//    GET  /api/config    {"name","tankDepth","upperGap","lowerGap",...}
//    POST /api/config    form fields: name, tankDepth, upperGap, lowerGap
//    GET|POST /api/restart  reboot (reply is sent first)
//  /api/* answers CORS requests, so the FirmwareManager web app can call it
//  from the browser.
//
//    Dashboard::begin()  register routes + start the server
//    Dashboard::loop()   serve clients; carries out a pending restart
//  HEADER-ONLY.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include "../config/globals.h"
#include "../config/settings.h"
#include "../logging/custom_mqtt_logging.h"
#include "../ota/ota_updates.h"
#include "../sensor/tank_sensor.h"

namespace Dashboard {

namespace detail {

inline ESP8266WebServer& server() {
  static ESP8266WebServer s(80);
  return s;
}

struct State {
  uint32_t restartAt = 0;  // 0 = none pending
  void (*beforeRestart)() = nullptr;
};
inline State& S() {
  static State s;
  return s;
}

// Single static page; live values come from /api/status.
static const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0a1020">
<title>Water Tank</title>
<style>
:root{--bg:#0a1020;--card:rgba(255,255,255,.045);--line:rgba(255,255,255,.08);--mute:#8a96ad;--fg:#eef2f8;
--acc:#38bdf8;--a1:#7dd3fc;--a2:#0284c7;--ok:#34d399;--warn:#fbbf24;--bad:#f87171}
*{box-sizing:border-box}
body{margin:0;font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:var(--fg);
background:radial-gradient(120% 60% at 50% -10%,#16264a 0%,var(--bg) 60%) fixed;min-height:100vh;
display:flex;justify-content:center;padding:28px 16px 40px;-webkit-font-smoothing:antialiased}
main{width:100%;max-width:400px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}
h1{font-size:1.25rem;margin:0;font-weight:650;letter-spacing:-.01em}
.sub{color:var(--mute);font-size:.75rem;margin-top:3px}
.pill{display:inline-flex;align-items:center;gap:6px;padding:5px 11px;border-radius:999px;font-size:.7rem;
font-weight:600;letter-spacing:.06em;background:var(--card);border:1px solid var(--line);color:var(--mute)}
.pill i{width:7px;height:7px;border-radius:50%;background:currentColor}
.card{background:var(--card);border:1px solid var(--line);border-radius:22px;padding:20px;margin-bottom:14px;
backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px)}
.hero{display:flex;gap:20px;align-items:center}
.tank{position:relative;flex:0 0 120px;height:220px;border-radius:26px;overflow:hidden;
background:linear-gradient(180deg,rgba(255,255,255,.03),rgba(255,255,255,.01));
border:1px solid rgba(255,255,255,.12);box-shadow:inset 0 0 24px rgba(0,0,0,.45)}
.water{position:absolute;left:0;right:0;bottom:0;height:0;background:linear-gradient(var(--a1),var(--a2));
transition:height 1.4s cubic-bezier(.22,1,.36,1)}
.water::before{content:"";position:absolute;left:-50%;top:-9px;width:200%;height:18px;border-radius:45%;
background:var(--a1);opacity:.55;animation:wave 6s linear infinite}
@keyframes wave{to{transform:translateX(25%)}}
.ticks{position:absolute;inset:0;pointer-events:none;
background:repeating-linear-gradient(to top,transparent 0 21px,rgba(255,255,255,.07) 21px 22px)}
.read{flex:1;min-width:0}
.pct{font-size:4rem;font-weight:750;line-height:1;letter-spacing:-.04em;font-variant-numeric:tabular-nums}
.pct small{font-size:1.6rem;font-weight:600;color:var(--mute);margin-left:2px}
.state{margin-top:12px}
.bar{height:6px;border-radius:6px;background:var(--line);margin-top:18px;overflow:hidden}
.bar div{height:100%;width:0;background:linear-gradient(90deg,var(--a2),var(--a1));transition:width 1.4s cubic-bezier(.22,1,.36,1)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.stat{background:rgba(255,255,255,.03);border:1px solid var(--line);border-radius:16px;padding:12px 14px}
.stat span{display:block;font-size:.68rem;color:var(--mute);text-transform:uppercase;letter-spacing:.08em}
.stat b{display:block;margin-top:5px;font-size:.98rem;font-weight:600;font-variant-numeric:tabular-nums;
white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
details summary{cursor:pointer;font-weight:600;list-style:none;display:flex;justify-content:space-between;align-items:center}
details summary::-webkit-details-marker{display:none}
details summary::after{content:"";width:8px;height:8px;border-right:2px solid var(--mute);border-bottom:2px solid var(--mute);
transform:rotate(45deg);transition:transform .25s;margin-right:4px}
details[open] summary::after{transform:rotate(225deg)}
label{display:block;font-size:.75rem;color:var(--mute);margin:16px 0 6px}
input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);background:rgba(0,0,0,.25);
color:var(--fg);font-size:1rem;outline:none;transition:border-color .2s,box-shadow .2s}
input:focus{border-color:var(--acc);box-shadow:0 0 0 3px rgba(56,189,248,.18)}
button{width:100%;margin-top:18px;padding:13px;border:0;border-radius:12px;font-size:.95rem;font-weight:600;cursor:pointer;
background:linear-gradient(135deg,#38bdf8,#0284c7);color:#fff;transition:filter .2s,transform .1s}
button:hover{filter:brightness(1.1)}button:active{transform:scale(.98)}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--mute);margin-top:10px}
button.ghost:hover{color:var(--bad);border-color:rgba(248,113,113,.4)}
.hint{font-size:.72rem;color:var(--mute);margin-top:10px;line-height:1.45}#msg{min-height:1em}
footer{text-align:center;color:var(--mute);font-size:.7rem;margin-top:18px;opacity:.7}
</style></head><body><main>
<header><div><h1 id="name">Water Tank</h1><div class="sub" id="age">connecting…</div></div>
<span class="pill" id="pill"><i></i><span id="state">LOADING</span></span></header>
<div class="card"><div class="hero">
<div class="tank"><div class="water" id="water"></div><div class="ticks"></div></div>
<div class="read"><div class="pct"><span id="pct">--</span><small>%</small></div>
<div class="sub state" id="dist">--</div>
<div class="bar"><div id="barf"></div></div></div></div></div>
<div class="card grid">
<div class="stat"><span>Range</span><b id="range">--</b></div>
<div class="stat"><span>Wi-Fi</span><b id="rssi">--</b></div>
<div class="stat"><span>Server</span><b id="mqtt">--</b></div>
<div class="stat"><span>Uptime</span><b id="up">--</b></div></div>
<div class="card"><details><summary>Settings</summary>
<form id="cfg">
<label for="n">Device name</label><input id="n" name="name" maxlength="32">
<label for="d">Tank depth: sensor to bottom (cm)</label><input id="d" name="tankDepth" type="number" min="10" max="1000" required>
<label for="u">Upper gap: sensor to 100% line (cm)</label><input id="u" name="upperGap" type="number" min="0" max="1000" required>
<label for="l">Lower gap: bottom to 0% line (cm)</label><input id="l" name="lowerGap" type="number" min="0" max="1000" required>
<p class="hint">0&ndash;100% is measured between the two gaps. Upper gap + lower gap must be smaller than the depth.</p>
<button type="submit">Save settings</button><p class="hint" id="msg"></p></form>
<button class="ghost" id="rst">Restart device</button></details></div>
<footer>Firmware <span id="fw">--</span></footer>
</main><script>
const $=i=>document.getElementById(i);let last=0;
const C={ok:['#7dd3fc','#0284c7','var(--ok)'],low:['#fde68a','#d97706','var(--warn)'],bad:['#fca5a5','#dc2626','var(--bad)'],off:['#64748b','#334155','var(--mute)']};
function up(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m'}
function theme(k){const c=C[k],r=document.documentElement.style;r.setProperty('--a1',c[0]);r.setProperty('--a2',c[1]);$('pill').style.color=c[2]}
async function poll(){try{const s=await(await fetch('/api/status')).json();last=Date.now();
const p=s.valid?Math.max(0,Math.min(100,Math.round(s.level))):null;$('pct').textContent=p===null?'--':p;
$('water').style.height=(p||0)+'%';$('barf').style.width=(p||0)+'%';
theme(p===null?'off':p>=60?'ok':p>=25?'low':'bad');
$('state').textContent=!s.valid?'NO READING':s.filling?'FILLING':p>=60?'GOOD':p>=25?'LOW':'CRITICAL';
$('dist').textContent=s.distance>0?s.distance.toFixed(1)+' cm to surface':'no distance reading';
$('range').textContent=s.fullAt+'–'+s.emptyAt+' cm';
$('rssi').textContent=s.rssi+' dBm'+(s.rssi>-60?' · strong':s.rssi>-75?' · ok':' · weak');
$('mqtt').textContent=s.mqtt?'Connected':'Offline';$('mqtt').style.color=s.mqtt?'var(--ok)':'var(--mute)';
$('up').textContent=up(s.uptime);$('fw').textContent=s.fw;
$('name').textContent=s.name;document.title=(p===null?'--':p+'%')+' · '+s.name}
catch(e){$('state').textContent='OFFLINE';theme('off')}}
async function loadCfg(){const c=await(await fetch('/api/config')).json();$('n').value=c.name;$('d').value=c.tankDepth;$('u').value=c.upperGap;$('l').value=c.lowerGap}
$('cfg').onsubmit=async e=>{e.preventDefault();const r=await fetch('/api/config',{method:'POST',body:new URLSearchParams(new FormData(e.target))});
$('msg').textContent=r.ok?'Saved.':'Error: '+await r.text();if(r.ok)poll()};
$('rst').onclick=async()=>{if(!confirm('Restart the device?'))return;await fetch('/api/restart',{method:'POST'});$('msg').textContent='Restarting…';setTimeout(()=>location.reload(),15000)};
setInterval(()=>{if(last)$('age').textContent='updated '+Math.round((Date.now()-last)/1000)+'s ago'},1000);
setInterval(()=>{if(!document.hidden)poll()},15000);document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});
poll();loadCfg();
</script></body></html>)HTML";

inline String jsonEscape(const char* s) {
  String o;
  for (; *s; ++s) {
    const char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<uint8_t>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

inline void cors() {
  ESP8266WebServer& s = server();
  s.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  s.sendHeader(F("Access-Control-Allow-Methods"), F("GET, POST, OPTIONS"));
  s.sendHeader(F("Access-Control-Allow-Headers"), F("Content-Type"));
  s.sendHeader(F("Cache-Control"), F("no-store"));
}

inline void sendJson(const String& body) {
  cors();
  server().send(200, F("application/json"), body);
}

inline void handleStatus() {
  const TankSensor::Reading& r = TankSensor::reading();
  char out[400];
  snprintf(out, sizeof out,
           "{\"name\":\"%s\",\"level\":%.1f,\"rawLevel\":%.1f,\"distance\":%.1f,\"valid\":%s,\"filling\":%s,"
           "\"fillRate\":%.2f,\"fullAt\":%.0f,\"emptyAt\":%.0f,\"readAgo\":%lu,\"uptime\":%lu,\"heap\":%u,"
           "\"rssi\":%d,\"mqtt\":%s,\"fw\":\"%s\",\"model\":\"" MODEL_NUMBER "\",\"mac\":\"%s\"}",
           jsonEscape(Settings::get().name).c_str(), r.level, r.rawLevel, r.distance, r.valid ? "true" : "false",
           r.filling ? "true" : "false", r.fillRate, Settings::fullDistance(), Settings::emptyDistance(),
           static_cast<unsigned long>((millis() - r.readAt) / 1000), static_cast<unsigned long>(millis() / 1000),
           ESP.getFreeHeap(), WiFi.RSSI(), RemoteLog::connected() ? "true" : "false",
           OtaUpdates::currentVersion().c_str(), WiFi.macAddress().c_str());
  sendJson(out);
}

inline void handleConfigGet() {
  const Settings::Config& c = Settings::get();
  char out[200];
  snprintf(out, sizeof out, "{\"name\":\"%s\",\"tankDepth\":%u,\"upperGap\":%u,\"lowerGap\":%u,\"ssid\":\"%s\"}",
           jsonEscape(c.name).c_str(), c.tankDepth, c.upperGap, c.lowerGap, jsonEscape(c.ssid).c_str());
  sendJson(out);
}

inline bool argCm(const char* key, uint16_t& out) {
  ESP8266WebServer& s = server();
  if (!s.hasArg(key)) return true;  // optional
  const String v = s.arg(key);
  const long n = v.toInt();
  if (!v.length() || n < 0 || n > 1000 || (n == 0 && v != "0")) return false;
  out = static_cast<uint16_t>(n);
  return true;
}

inline void handleConfigPost() {
  ESP8266WebServer& s = server();
  Settings::Config& c = Settings::get();
  uint16_t depth = c.tankDepth, upper = c.upperGap, lower = c.lowerGap;
  cors();
  if (!argCm("tankDepth", depth) || !argCm("upperGap", upper) || !argCm("lowerGap", lower)) {
    s.send(400, F("text/plain"), F("values must be whole cm between 0 and 1000"));
    return;
  }
  if (depth < 10 || upper + lower + 2 > depth) {
    s.send(400, F("text/plain"), F("upper gap + lower gap must be smaller than the tank depth"));
    return;
  }
  const bool scaleChanged = depth != c.tankDepth || upper != c.upperGap || lower != c.lowerGap;
  if (s.hasArg("name") && s.arg("name").length()) {
    memset(c.name, 0, sizeof c.name);
    strncpy(c.name, s.arg("name").c_str(), sizeof c.name - 1);
  }
  c.tankDepth = depth;
  c.upperGap = upper;
  c.lowerGap = lower;
  Settings::save();
  if (scaleChanged) TankSensor::recalibrate();
  LOGI("WEB", "settings saved from %s", s.client().remoteIP().toString().c_str());
  handleConfigGet();
}

inline void handleRestart() {
  cors();
  server().send(200, F("application/json"), F("{\"ok\":true,\"message\":\"restarting\"}"));
  LOGW("WEB", "restart requested via /api/restart from %s", server().client().remoteIP().toString().c_str());
  S().restartAt = millis() + 500;  // let the reply leave first
  if (!S().restartAt) S().restartAt = 1;
}

}  // namespace detail

/** Called right before an /api/restart reboot. */
inline void setBeforeRestart(void (*fn)()) { detail::S().beforeRestart = fn; }

inline void begin() {
  ESP8266WebServer& s = detail::server();
  s.on("/", HTTP_GET, []() {
    detail::server().sendHeader(F("Cache-Control"), F("no-cache"));
    detail::server().send_P(200, "text/html", detail::kPage);
  });
  s.on("/api/status", HTTP_GET, detail::handleStatus);
  s.on("/api/config", HTTP_GET, detail::handleConfigGet);
  s.on("/api/config", HTTP_POST, detail::handleConfigPost);
  s.on("/api/restart", HTTP_GET, detail::handleRestart);
  s.on("/api/restart", HTTP_POST, detail::handleRestart);
  s.onNotFound([]() {
    ESP8266WebServer& s = detail::server();
    if (s.method() == HTTP_OPTIONS) {  // CORS preflight
      detail::cors();
      s.send(204);
      return;
    }
    s.send(404, F("text/plain"), F("not found"));
  });
  s.begin();
  LOGI("WEB", "dashboard on http://%s.local and http://%s", MDNS_NAME, WiFi.localIP().toString().c_str());
}

inline void loop() {
  detail::server().handleClient();
  const uint32_t at = detail::S().restartAt;
  if (at && static_cast<int32_t>(millis() - at) >= 0) {
    detail::S().restartAt = 0;
    if (detail::S().beforeRestart) detail::S().beforeRestart();
    Diag::restart("restart via /api/restart");
  }
}

}  // namespace Dashboard
