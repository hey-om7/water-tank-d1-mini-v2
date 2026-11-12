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
<title>Water Tank</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--line:#334155;--mute:#94a3b8;--fg:#e2e8f0;--c:#0ea5e9}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg);
min-height:100vh;display:flex;justify-content:center;padding:24px 16px}
main{width:100%;max-width:380px}
header{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:16px}
h1{font-size:1.1rem;margin:0;font-weight:600}.mute{color:var(--mute);font-size:.8rem}
.card{background:var(--card);border-radius:16px;padding:20px;margin-bottom:12px}
.tank{position:relative;height:260px;border:2px solid var(--line);border-radius:14px;overflow:hidden;background:#0b1222}
.water{position:absolute;left:0;right:0;bottom:0;height:0;background:linear-gradient(#38bdf8,#0369a1);transition:height 1.2s ease,background .6s}
.pct{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;text-shadow:0 1px 6px #0009}
.pct b{font-size:3.6rem;font-weight:800;line-height:1}.pct span{font-size:.85rem;margin-top:6px;letter-spacing:.08em}
.row{display:flex;justify-content:space-between;padding:6px 0;font-size:.9rem;border-bottom:1px solid var(--line)}
.row:last-child{border:0}.row span:first-child{color:var(--mute)}
details summary{cursor:pointer;font-weight:600;list-style:none}details summary::after{content:" \25BE";color:var(--mute)}
label{display:block;font-size:.8rem;color:var(--mute);margin:14px 0 6px}
input{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:#0b1222;color:#fff;font-size:1rem}
button{width:100%;margin-top:16px;padding:11px;border:0;border-radius:8px;background:var(--c);color:#fff;font-size:1rem;cursor:pointer}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--mute)}
.hint{font-size:.75rem;color:var(--mute);margin-top:10px}#msg{min-height:1em}
</style></head><body><main>
<header><h1 id="name">Water Tank</h1><span class="mute" id="age"></span></header>
<div class="card"><div class="tank"><div class="water" id="water"></div>
<div class="pct"><b id="pct">--</b><span id="state">LOADING</span></div></div></div>
<div class="card">
<div class="row"><span>Distance</span><span id="dist">--</span></div>
<div class="row"><span>Measured range</span><span id="range">--</span></div>
<div class="row"><span>Wi-Fi</span><span id="rssi">--</span></div>
<div class="row"><span>Server</span><span id="mqtt">--</span></div>
<div class="row"><span>Uptime</span><span id="up">--</span></div>
<div class="row"><span>Firmware</span><span id="fw">--</span></div></div>
<div class="card"><details><summary>Settings</summary>
<form id="cfg">
<label for="n">Device name</label><input id="n" name="name" maxlength="32">
<label for="d">Tank depth: sensor to bottom (cm)</label><input id="d" name="tankDepth" type="number" min="10" max="1000" required>
<label for="u">Upper gap: sensor to 100% line (cm)</label><input id="u" name="upperGap" type="number" min="0" max="1000" required>
<label for="l">Lower gap: bottom to 0% line (cm)</label><input id="l" name="lowerGap" type="number" min="0" max="1000" required>
<p class="hint">0&ndash;100% is measured between the two gaps. Upper gap + lower gap must be smaller than the depth.</p>
<button type="submit">Save</button><p class="hint" id="msg"></p></form>
<button class="ghost" id="rst">Restart device</button></details></div>
</main><script>
const $=i=>document.getElementById(i);let last=0;
function up(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m'}
function color(p){return p>=60?'linear-gradient(#38bdf8,#0369a1)':p>=25?'linear-gradient(#fbbf24,#b45309)':'linear-gradient(#f87171,#b91c1c)'}
async function poll(){try{const s=await(await fetch('/api/status')).json();last=Date.now();
const p=s.valid?Math.round(s.level):null;$('pct').textContent=p===null?'--':p+'%';
$('water').style.height=(p||0)+'%';$('water').style.background=color(p||0);
$('state').textContent=!s.valid?'NO READING':s.filling?'FILLING':p>=60?'GOOD':p>=25?'LOW':'CRITICAL';
$('dist').textContent=s.distance>0?s.distance.toFixed(1)+' cm':'--';
$('range').textContent=s.fullAt+' – '+s.emptyAt+' cm';$('rssi').textContent=s.rssi+' dBm';
$('mqtt').textContent=s.mqtt?'connected':'offline';$('up').textContent=up(s.uptime);$('fw').textContent=s.fw;
$('name').textContent=s.name;document.title=(p===null?'--':p+'%')+' · '+s.name}catch(e){$('state').textContent='OFFLINE'}}
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
