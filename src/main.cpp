/**
 * SCAVENGER — ESP32 Scavenger Hunt Kiosk (Codeword Only + Admin Auth)
 * - Setup mode: passworded AP for organizers + Admin UI with form rows
 * - Game mode: OPEN AP (no password), captive portal pushes players to /app
 * - Auto-reset-on-flash: clears admin hash when FW_VERSION changes
 * - Factory reset endpoint to wipe FS and reboot
 * - Player portal uses ONLY a text input for codewords (no camera/QR)
 * - Admin is gated with HTTP Basic Auth after first-time setup
 *
 * PlatformIO: Arduino framework 2.0.x, LittleFS, AsyncWebServer, ArduinoJson
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <DNSServer.h>
#include <vector>
#include <algorithm>

// ------------------ FIRMWARE VERSION & RESET POLICY ------------------
#define FW_VERSION  (__DATE__ " " __TIME__)

// Reset policy toggles when FW_VERSION changes:
#define RESET_ADMIN_ON_VERSION        1   // clear admin password
#define FORCE_SETUP_MODE_ON_VERSION   1   // go back to setup mode
#define WIPE_CHECKPOINTS_ON_VERSION   0   // delete /checkpoints.json
#define WIPE_TEAMS_ON_VERSION         0   // delete /teams.json

// ------------------ CONFIG ------------------
#define DEFAULT_SETUP_SSID "SCAVENGER-SETUP"
#define DEFAULT_SETUP_PASS "organizer123"
#define DEFAULT_GAME_SSID  "SCAVENGER"
#define DEFAULT_GAME_PASS  ""         // OPEN network in game mode

// Functional limits
#define TOKEN_MAXLEN     64
#define NAME_MAXLEN      40
#define PIN_MINLEN        4
#define PIN_MAXLEN        6
#define LEADERBOARD_SIZE 20

// Files
static const char* FILE_CONFIG       = "/config.json";
static const char* FILE_CHECKPOINTS  = "/checkpoints.json";
static const char* FILE_TEAMS        = "/teams.json";

// ---------------------------------------------------------

// Types
struct Checkpoint {
  String id;
  String name;
  String token_text; // exact codeword
  int points = 10;
};

struct Team {
  String id;
  String name;
  String pin_hash;
  std::vector<String> found; // checkpoint ids
  int points = 0;
  uint32_t created_at = 0;
};

enum Mode { MODE_SETUP, MODE_GAME };

// Runtime state
AsyncWebServer server(80);
DNSServer dnsServer;           // Captive portal DNS
Mode mode = MODE_SETUP;

std::vector<Checkpoint> g_checkpoints;
std::vector<Team> g_teams;

// Forward decls
void startSetupAP();
void startGameAP();
void startCaptivePortalDNSOnly();
void stopCaptivePortalDNSOnly();
void registerCaptiveHandlersOnce(AsyncWebServer& s);
void mountFS();
void loadAll();
bool saveCheckpoints();
bool loadCheckpoints();
bool saveTeams();
bool loadTeams();
bool saveConfig();
bool loadConfig(Mode &outMode, String &adminHash, String &storedVersion);
bool consttime_eq(const String& a, const String& b);
String sha256Hex(const String& in);
String newId(const char* prefix);
void enterSetupMode();
void enterGameMode();
void switchAPNow(Mode m);
void setupRoutes();
void updatePointsFromFound(Team& t);
Team* findTeamById(const String& id);
Team* findTeamByName(const String& nm);
Checkpoint* findCheckpointByToken(const String& token);
Checkpoint* findCheckpointById(const String& id);
bool teamFoundHas(const Team& t, const String& chkId);
bool teamAddFound(Team& t, const String& chkId, int points);
String htmlIndex();
String htmlAdmin();
String pwaManifest();
String pwaServiceWorker();
String htmlCaptive();
void addCaptiveRoute();
void applyVersionResetIfNeeded(const String& storedVersion);
void factoryReset(bool wipeAll);
bool saneToken(const String& s);
String sanitizeName(const String& s);

// Config
struct Config {
  String admin_hash;  // sha256
  String setup_ssid = DEFAULT_SETUP_SSID;
  String setup_pass = DEFAULT_SETUP_PASS;
  String game_ssid  = DEFAULT_GAME_SSID;
  String game_pass  = DEFAULT_GAME_PASS;   // "" => OPEN
  String fw_version = "";                  // persisted firmware version
  Mode mode = MODE_SETUP;
} g_config;

// ------------------ JSON helpers ------------------
static inline String JV_toString(JsonVariant v, const char* def = "") {
  const char* s = v | def;
  return String(s ? s : def);
}

// ------------------ FS helpers ------------------

bool readFileToString(const char* path, String &out) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out.reserve(f.size()+8);
  out = "";
  while (f.available()) out += char(f.read());
  f.close();
  return true;
}

bool writeStringToFile(const char* path, const String &data) {
  String tmp = String(path) + ".tmp";
  File t = LittleFS.open(tmp.c_str(), "w");
  if (!t) return false;
  size_t n = t.print(data);
  t.close();
  if (n != data.length()) { LittleFS.remove(tmp.c_str()); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp.c_str(), path);
}

void removeIfExists(const char* path){
  if (LittleFS.exists(path)) LittleFS.remove(path);
}

// Load/save config
bool saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["admin_hash"] = g_config.admin_hash;
  doc["setup_ssid"] = g_config.setup_ssid;
  doc["setup_pass"] = g_config.setup_pass;
  doc["game_ssid"]  = g_config.game_ssid;
  doc["game_pass"]  = ""; // force OPEN for game mode on save
  doc["mode"]       = (g_config.mode == MODE_SETUP) ? "setup" : "game";
  doc["fw_version"] = g_config.fw_version;
  String out;
  serializeJson(doc, out);
  return writeStringToFile(FILE_CONFIG, out);
}

bool loadConfig(Mode &outMode, String &adminHash, String &storedVersion) {
  String s;
  if (!readFileToString(FILE_CONFIG, s)) return false;
  DynamicJsonDocument doc(2048);
  auto err = deserializeJson(doc, s);
  if (err) return false;

  g_config.admin_hash = JV_toString(doc["admin_hash"], "");
  g_config.setup_ssid = JV_toString(doc["setup_ssid"], DEFAULT_SETUP_SSID);
  g_config.setup_pass = JV_toString(doc["setup_pass"], DEFAULT_SETUP_PASS);
  g_config.game_ssid  = JV_toString(doc["game_ssid"],  DEFAULT_GAME_SSID);
  // Force open AP by ignoring stored game_pass
  g_config.game_pass  = "";
  g_config.fw_version = JV_toString(doc["fw_version"], "");
  String m = JV_toString(doc["mode"], "setup");
  outMode = (m == "game") ? MODE_GAME : MODE_SETUP;

  adminHash = g_config.admin_hash;
  storedVersion = g_config.fw_version;
  return true;
}

// Load/save checkpoints
bool saveCheckpoints() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &c : g_checkpoints) {
    JsonObject o = arr.createNestedObject();
    o["id"] = c.id;
    o["name"] = c.name;
    o["token_text"] = c.token_text;
    o["points"] = c.points;
  }
  String out;
  serializeJson(doc, out);
  return writeStringToFile(FILE_CHECKPOINTS, out);
}

bool loadCheckpoints() {
  String s; g_checkpoints.clear();
  if (!readFileToString(FILE_CHECKPOINTS, s)) return false;
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, s)) return false;
  for (JsonObject o : doc.as<JsonArray>()) {
    Checkpoint c;
    c.id         = JV_toString(o["id"], "");
    c.name       = JV_toString(o["name"], "");
    c.token_text = JV_toString(o["token_text"], "");
    c.points     = int(o["points"] | 10);
    g_checkpoints.push_back(c);
  }
  return true;
}

// Load/save teams
bool saveTeams() {
  DynamicJsonDocument doc(16384);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &t : g_teams) {
    JsonObject o = arr.createNestedObject();
    o["id"] = t.id;
    o["name"] = t.name;
    o["pin_hash"] = t.pin_hash;
    o["points"] = t.points;
    o["created_at"] = t.created_at;
    JsonArray f = o.createNestedArray("found");
    for (auto &x : t.found) f.add(x);
  }
  String out; serializeJson(doc, out);
  return writeStringToFile(FILE_TEAMS, out);
}

bool loadTeams() {
  String s; g_teams.clear();
  if (!readFileToString(FILE_TEAMS, s)) return false;
  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, s)) return false;
  for (JsonObject o : doc.as<JsonArray>()) {
    Team t;
    t.id = JV_toString(o["id"], "");
    t.name = JV_toString(o["name"], "");
    t.pin_hash = JV_toString(o["pin_hash"], "");
    t.points = int(o["points"] | 0);
    t.created_at = uint32_t(o["created_at"] | 0);
    if (o.containsKey("found")) {
      for (JsonVariant v : o["found"].as<JsonArray>()) t.found.push_back(JV_toString(v, ""));
    }
    g_teams.push_back(t);
  }
  return true;
}

void loadAll() {
  String ah; Mode m; String storedVer;
  bool have = loadConfig(m, ah, storedVer);
  if (!have) {
    g_config.admin_hash = "";
    g_config.mode = MODE_SETUP;
    g_config.fw_version = FW_VERSION;
    saveConfig();
  } else {
    g_config.mode = m;
    applyVersionResetIfNeeded(storedVer);
  }
  // Load data files (may be wiped by version reset)
  loadCheckpoints();
  loadTeams();
}

// ------------------ Version reset logic ------------------

void applyVersionResetIfNeeded(const String& storedVersion) {
  if (storedVersion == String(FW_VERSION)) return;

  Serial.printf("[FW] Version change detected: '%s' -> '%s'\n",
                storedVersion.c_str(), FW_VERSION);

  if (RESET_ADMIN_ON_VERSION) {
    g_config.admin_hash = "";
    Serial.println("[FW] admin_hash cleared");
  }
  if (FORCE_SETUP_MODE_ON_VERSION) {
    g_config.mode = MODE_SETUP;
    Serial.println("[FW] forced MODE_SETUP");
  }
  if (WIPE_CHECKPOINTS_ON_VERSION) {
    removeIfExists(FILE_CHECKPOINTS);
    Serial.println("[FW] wiped checkpoints");
  }
  if (WIPE_TEAMS_ON_VERSION) {
    removeIfExists(FILE_TEAMS);
    Serial.println("[FW] wiped teams");
  }

  g_config.fw_version = FW_VERSION;
  saveConfig();
}

// ------------------ utils ------------------

String sha256Hex(const String& in) {
  byte out[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)in.c_str(), in.length());
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  static const char* hex="0123456789abcdef";
  String h; h.reserve(64);
  for (int i=0;i<32;i++){ h+=hex[out[i]>>4]; h+=hex[out[i]&0xF]; }
  return h;
}

bool consttime_eq(const String& a, const String& b) {
  size_t la = a.length(), lb = b.length();
  size_t l = (la>lb)?la:lb;
  byte diff = 0;
  for (size_t i=0;i<l;i++) {
    char ca = (i<la)?a[i]:0;
    char cb = (i<lb)?b[i]:0;
    diff |= (ca ^ cb);
  }
  return diff == 0 && la == lb;
}

String newId(const char* prefix) {
  char buf[16];
  uint32_t r = esp_random();
  snprintf(buf, sizeof(buf), "%s%03u", prefix, (unsigned)(r % 1000));
  return String(buf);
}

void updatePointsFromFound(Team& t) {
  int pts = 0;
  for (auto &cid : t.found) {
    auto *c = findCheckpointById(cid);
    if (c) pts += c->points;
  }
  t.points = pts;
}

Team* findTeamById(const String& id) {
  for (auto &t : g_teams) if (t.id == id) return &t;
  return nullptr;
}
Team* findTeamByName(const String& nm) {
  for (auto &t : g_teams) if (t.name == nm) return &t;
  return nullptr;
}
Checkpoint* findCheckpointByToken(const String& token) {
  for (auto &c : g_checkpoints) if (consttime_eq(c.token_text, token)) return &c;
  return nullptr;
}
Checkpoint* findCheckpointById(const String& id) {
  for (auto &c : g_checkpoints) if (c.id == id) return &c;
  return nullptr;
}

bool teamFoundHas(const Team& t, const String& chkId) {
  for (auto &x : t.found) if (x == chkId) return true;
  return false;
}

bool teamAddFound(Team& t, const String& chkId, int points) {
  if (teamFoundHas(t, chkId)) return false;
  t.found.push_back(chkId);
  updatePointsFromFound(t);
  (void)points; // points recomputed above
  return true;
}

// ------------------ Security helpers (GLOBAL SCOPE) ------------------

bool saneToken(const String& s) {
  if (s.length() < 1 || s.length() > TOKEN_MAXLEN) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '/' || c == ' ';
    if (!ok) return false;
  }
  return true;
}

String sanitizeName(const String& s) {
  String out;
  for (size_t i = 0; i < s.length() && out.length() < NAME_MAXLEN; i++) {
    char c = s[i];
    // strip dangerous or control chars
    if (c == '<' || c == '>' || c == '"' || c == '\'' || c == '&') continue;
    if (c < 32) continue;
    out += c;
  }
  out.trim();
  return out;
}

// ------------------ Wi-Fi/AP + Captive Portal (fixed) ------------------

void startCaptivePortalDNSOnly() {
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
}

void stopCaptivePortalDNSOnly() { dnsServer.stop(); }

// Register captive handlers ONCE. They choose landing based on current mode at request time.
void registerCaptiveHandlersOnce(AsyncWebServer& s) {
  static bool done = false;
  if (done) return;
  auto redirectToCurrentLanding = [](AsyncWebServerRequest *r){
    const char* landing = (g_config.mode == MODE_SETUP) ? "/admin" : "/captive";
    r->redirect(landing);
  };
  s.on("/generate_204",        HTTP_ANY, redirectToCurrentLanding); // Android
  s.on("/hotspot-detect.html", HTTP_ANY, redirectToCurrentLanding); // iOS/macOS
  s.on("/ncsi.txt",            HTTP_ANY, redirectToCurrentLanding); // Windows
  s.on("/connecttest.txt",     HTTP_ANY, redirectToCurrentLanding); // Others
  s.onNotFound(redirectToCurrentLanding);
  done = true;
}

void startSetupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_config.setup_ssid.c_str(), g_config.setup_pass.c_str(), 6, false, 8);
  Serial.printf("[WiFi] Setup AP: %s (pass: %s) IP: %s\n",
    g_config.setup_ssid.c_str(), g_config.setup_pass.c_str(), WiFi.softAPIP().toString().c_str());
  startCaptivePortalDNSOnly();
}

void startGameAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_config.game_ssid.c_str(), NULL, 6, false, 12); // OPEN
  Serial.printf("[WiFi] Game AP: %s (OPEN) IP: %s\n",
    g_config.game_ssid.c_str(), WiFi.softAPIP().toString().c_str());
  startCaptivePortalDNSOnly();
}

void switchAPNow(Mode m) {
  WiFi.softAPdisconnect(true);
  delay(100);
  stopCaptivePortalDNSOnly();
  if (m == MODE_SETUP) startSetupAP(); else startGameAP();
}

// ------------------ Web helpers ------------------

void sendJSON(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
  String out; serializeJson(doc, out);
  req->send(code, "application/json; charset=utf-8", out);
}

String getBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  String s; s.reserve(len+1);
  for (size_t i=0;i<len;i++) s += (char)data[i];
  return s;
}

// -------- Admin auth helpers (HTTP Basic) --------
static String getHeader(AsyncWebServerRequest *req, const char* name) {
  if (req->hasHeader(name)) return req->getHeader(name)->value();
  return String();
}

static bool parseBasicAuth(const String& auth, String &user, String &pass) {
  if (!auth.startsWith("Basic ")) return false;
  String b64 = auth.substring(6);
  size_t needed = 0;
  // First call to learn required length
  mbedtls_base64_decode(nullptr, 0, &needed,
                        (const unsigned char*)b64.c_str(), b64.length());
  if (needed == 0) return false;
  std::vector<unsigned char> buf(needed + 1);
  size_t olen = 0;
  if (mbedtls_base64_decode(buf.data(), needed, &olen,
                            (const unsigned char*)b64.c_str(), b64.length()) != 0) return false;
  buf[olen] = 0;
  String decoded = String((const char*)buf.data());
  int colon = decoded.indexOf(':');
  if (colon < 0) return false;
  user = decoded.substring(0, colon);
  pass = decoded.substring(colon + 1);
  return true;
}

static bool adminGuard(AsyncWebServerRequest *req) {
  // Allow first-time setup without auth
  if (g_config.admin_hash.length() == 0) return true;

  // Check Basic auth
  String u, p;
  String auth = getHeader(req, "Authorization"); // helper you already defined
  if (parseBasicAuth(auth, u, p)) {
    if (consttime_eq(sha256Hex(p), g_config.admin_hash)) return true;
  }

  // Challenge with WWW-Authenticate on a response object
  AsyncWebServerResponse* r =
      req->beginResponse(401, "text/plain", "Authentication required");
  r->addHeader("WWW-Authenticate", "Basic realm=\"Scavenger Admin\"");
  req->send(r);
  return false;
}


// ------------------ HTML & PWA (raw strings) ------------------

// Player portal: codeword-only flow
static const char HTML_INDEX[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<link rel="manifest" href="/manifest.webmanifest">
<title>Scavenger — Player Portal</title>
<style>
:root{--b:#222;--t:#fff;--mut:#666}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Helvetica,Arial,sans-serif;background:#fafafa;margin:16px;color:#111}
h1{font-size:1.6rem;margin:0 0 12px}
.card{background:#fff;border:1px solid #ddd;border-radius:12px;padding:12px;margin:10px 0}
input,button{font-size:1rem;padding:10px;border-radius:10px;border:1px solid #bbb}
button{background:#222;color:#fff;border:0;cursor:pointer;transition:transform .04s ease,filter .04s ease,box-shadow .08s ease}
button:active{transform:translateY(1px);filter:brightness(0.92)}
button.ghost{background:#f3f3f3;color:#111;border:1px solid #ccc}
button.ghost:active{filter:brightness(0.95);transform:translateY(1px)}
.row{display:flex;gap:8px;flex-wrap:wrap}
.badge{background:#eee;border-radius:999px;padding:2px 8px;margin-left:6px}
.small{color:#555}
table{width:100%;border-collapse:collapse}
th,td{border-bottom:1px solid #eee;padding:8px;text-align:left}
th{background:#f9f9f9}
.status-found{color:green;font-weight:600}
.status-miss{color:#b00;font-weight:600}
.footer{color:#777;font-size:.9rem;margin-top:8px}
.hint{font-size:.95rem;color:#333}
.hide{display:none}
</style>
</head><body>
<h1>Scavenger — Player Portal</h1>

<div class="card">
  <h3>How it works</h3>
  <p class="small">
    Connect to the event Wi-Fi, create a team (or log in), and type the <b>codeword</b> printed at each checkpoint.
  </p>
</div>

<div class="card" id="auth">
  <h3>Register / Login</h3>
  <div class="row">
    <input id="name" placeholder="Team name" maxlength="40">
    <input id="pin" placeholder="PIN (4-6)" type="password" maxlength="6">
    <button onclick="reg()">Register</button>
    <button class="ghost" onclick="login()">Login</button>
  </div>
  <div id="me" class="small"></div>
</div>

<div class="card">
  <h3>Leaderboard <span class="badge" id="ts"></span></h3>
  <div id="lb">Loading…</div>
</div>

<div class="card hide" id="itemsCard">
  <h3>Your Items</h3>
  <table>
    <thead><tr><th>Item</th><th>Points</th><th>Status</th></tr></thead>
    <tbody id="itemsBody"></tbody>
  </table>

  <div style="margin-top:12px">
    <h3>Enter codeword</h3>
    <div class="row">
      <input id="codeword" placeholder="Type codeword here" maxlength="64" style="flex:1;min-width:220px">
      <button onclick="submitCode()">Submit</button>
    </div>
    <div class="hint" style="margin-top:6px">Tip: Codes are not case-sensitive and may include numbers or dashes.</div>
  </div>
</div>

<script>
var team_id=null, team_name="";

function id(x){return document.getElementById(x);}
function val(x){var el=id(x); return el?el.value:'';}

function j(p,u,f){
  fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)})
    .then(function(r){return r.json();})
    .then(f)
    .catch(function(err){toast((err&&err.message)||'Network error');});
}
function t(u,f){
  fetch(u)
    .then(function(r){return r.json();})
    .then(f)
    .catch(function(err){toast((err&&err.message)||'Network error');});
}

function reg(){
  j({team_name:val('name'),pin:val('pin')},'/api/register',function(r){
    if(r && r.ok){ team_id=r.team_id; team_name=val('name'); onAuth(); }
    else { toast(JSON.stringify(r)); }
  });
}
function login(){
  j({team_name:val('name'),pin:val('pin')},'/api/login',function(r){
    if(r && r.ok){ team_id=r.team_id; team_name=val('name'); onAuth(); }
    else { toast(JSON.stringify(r)); }
  });
}

function onAuth(){
  id('me').textContent='Logged in as: '+team_name;
  id('itemsCard').classList.remove('hide');
  loadTeamItems();
}

function loadLB(){
  t('/api/leaderboard',function(r){
    var h='<ol>';
    var teams=(r&&r.teams)||[];
    if(teams.length===0){ h+='<li>No teams yet</li>'; }
    for(var i=0;i<teams.length;i++){
      var x=teams[i];
      h+='<li>'+escapeHtml(x.name)+' — '+(x.points||0)+' pts ('+(x.found||0)+')</li>';
    }
    h+='</ol>';
    id('lb').innerHTML=h;
    id('ts').textContent=new Date().toLocaleTimeString();
  });
}

function loadTeamItems(){
  if(!team_id) return;
  j({team_id:team_id},'/api/team/items',function(r){
    var tb=id('itemsBody');
    tb.innerHTML='';
    var items=(r&&r.items)||[];
    for(var i=0;i<items.length;i++){
      var it=items[i];
      var found=!!it.found;
      var st=found ? '<span class="status-found">Found</span>' : '<span class="status-miss">Missing</span>';
      tb.insertAdjacentHTML('beforeend',
        '<tr><td>'+escapeHtml(it.name)+'</td><td>'+(it.points||0)+'</td><td>'+st+'</td></tr>');
    }
  });
}

function submitCode(){
  var token = val('codeword').trim();
  if(!team_id){ toast('Please register/login first.'); return; }
  if(!token){ toast('Enter a codeword'); return; }
  busy(true);
  j({team_id:team_id, token:token},'/api/team/submit_code',function(r){
    busy(false);
    if(r && r.ok){
      toast('+'+(r.awarded||0)+' pts! Total: '+(r.total||0));
      id('codeword').value='';
      loadTeamItems(); loadLB();
    }else if(r && r.duplicate){
      toast('Already found.');
      loadTeamItems();
    }else{
      toast(JSON.stringify(r));
    }
  });
}

// --- tiny UX helpers ---
var _busy=0;
function busy(on){
  _busy = on ? (_busy+1) : Math.max(0,_busy-1);
  document.body.style.cursor = _busy ? 'progress' : '';
}
function toast(msg){
  try{ console.log('[toast]', msg); alert(msg); }catch(e){}
}
function escapeHtml(s){
  if(s==null) return '';
  return String(s)
    .replace(/&/g,'&amp;')
    .replace(/</g,'&lt;')
    .replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;')
    .replace(/'/g,'&#39;');
}

// init
loadLB();
setInterval(loadLB,6000);
</script>

</body></html>)HTML";

// Admin (wording tuned)
static const char HTML_ADMIN[] PROGMEM = R"ADMIN(<!doctype html><html><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Scavenger Admin</title>
<style>
body{font-family:system-ui;margin:16px}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}
input,button{font-size:1rem;padding:8px;border-radius:8px;border:1px solid #bbb}
button{background:#222;color:#fff;border:0;cursor:pointer;transition:transform .04s ease,filter .04s ease}
button:active{transform:translateY(1px);filter:brightness(0.92)}
table{width:100%;border-collapse:collapse;margin-top:8px}
th,td{border:1px solid #ddd;padding:6px;text-align:left}
.small{color:#555}
.badge{background:#eee;border-radius:999px;padding:2px 8px;margin-left:6px}
</style>

</head><body>
<h1>Admin</h1>

<div id="first">
  <p><b>First-time setup:</b> set password</p>
  <div class="row">
    <input id="pass" type="password" placeholder="New admin password">
    <button onclick="setup()">Save</button>
  </div>
</div>

<hr>
<h3>Game Wi-Fi</h3>
<p class="small">This SSID will be used when switching to GAME mode (open network, no password).</p>
<div class="row">
  <input id="game_ssid" placeholder="Game SSID">
  <button onclick="saveSSID()">Save SSID</button>
</div>

<hr>
<h3>Checkpoints <span class="badge" id="count"></span></h3>
<p class="small">Add one row per item. <b>Token</b> is the exact codeword. Points default to 10.</p>
<div class="row">
  <button onclick="addRow()">Add item</button>
  <button onclick="save()">Save all</button>
  <button onclick="reload()">Reload</button>
  <button class="ghost" onclick="factory(false)">Reset to organizer (keep items)</button>
  <button class="ghost" onclick="factory(true)">Factory reset (wipe all)</button>
</div>
<table id="tbl">
  <thead><tr><th>Name</th><th>Token (codeword)</th><th>Points</th><th></th></tr></thead>
  <tbody id="rows"></tbody>
</table>

<hr>
<h3>Mode</h3>
<div class="row">
  <button onclick="mode('setup')">Switch to SETUP mode</button>
  <button onclick="mode('game')">Switch to GAME mode</button>
</div>

<script>
function setup(){
  fetch('/api/admin/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({pass:document.getElementById('pass').value})})
    .then(r=>r.json()).then(x=>alert(JSON.stringify(x)));
}

function saveSSID(){
  const ssid = document.getElementById('game_ssid').value.trim();
  fetch('/api/admin/game_ssid',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid})})
    .then(r=>r.json()).then(x=>alert(JSON.stringify(x)));
}

function rowHtml(n='',t='',p=10){
  return `<tr>
    <td><input value="${n}" placeholder="Name" maxlength="40"></td>
    <td><input value="${t}" placeholder="Token (codeword)" maxlength="64"></td>
    <td><input value="${p}" type="number" min="1" max="1000" style="width:90px"></td>
    <td><button onclick="this.closest('tr').remove()">✕</button></td>
  </tr>`;
}

function addRow(){ document.getElementById('rows').insertAdjacentHTML('beforeend', rowHtml()); }

function reload(){
  fetch('/api/admin/checkpoints').then(r=>r.json()).then(x=>{
    const tb=document.getElementById('rows'); tb.innerHTML='';
    const items=(x.items||[]);
    items.forEach(i=>tb.insertAdjacentHTML('beforeend', rowHtml(i.name,i.token_text,i.points)));
    document.getElementById('count').textContent = items.length + ' items';
  });
  fetch('/api/admin/status').then(r=>r.json()).then(x=>{
    if (x.game_ssid) document.getElementById('game_ssid').value=x.game_ssid;
  });
}

function save(){
  const rows=[...document.querySelectorAll('#rows tr')];
  const items = rows.map(tr=>{
    const ins=[...tr.querySelectorAll('input')];
    const n=ins[0].value.trim(), t=ins[1].value.trim(), p=parseInt(ins[2].value||'10')||10;
    return {id:'',name:n,token_text:t,points:p};
  }).filter(i=>i.name && i.token_text);
  fetch('/api/admin/checkpoints',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(items)})
    .then(r=>r.json()).then(x=>{ alert(JSON.stringify(x)); reload(); });
}

function mode(m){
  fetch('/api/admin/mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})})
    .then(r=>r.json()).then(x=>alert(JSON.stringify(x)));
}

function factory(all){
  fetch('/api/admin/factory_reset',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({wipe_all:all})})
    .then(r=>r.json()).then(x=>alert(JSON.stringify(x)));
}

reload();
</script>
</body></html>)ADMIN";

// ---------- Captive landing page (auto-redirects to /app) ----------
static const char HTML_CAPTIVE[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Scavenger Hunt Wi-Fi</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Helvetica,Arial,sans-serif;
       text-align:center;padding:2rem;background:#fafafa;color:#111}
  a.button{display:inline-block;margin-top:1.25rem;padding:1rem 1.5rem;background:#222;color:#fff;
           border-radius:12px;text-decoration:none;font-size:1.05rem;transition:transform .04s ease,filter .04s ease}
  a.button:active{transform:translateY(1px);filter:brightness(0.92)}
  p{max-width:460px;margin:1rem auto;color:#555;line-height:1.4}
  code{background:#eee;padding:.1rem .3rem;border-radius:6px}
</style>
<script>
setTimeout(()=>{ try{ location.replace('/app'); }catch(e){} }, 600);
</script>
</head><body>
  <h1>You’re connected 🎉</h1>
  <p>This is the Wi-Fi sign-in screen. The game portal should open automatically. If not, tap below.</p>
  <a class="button" href="/app" rel="noopener">Open Game Portal</a>
  <p>If that doesn’t open, manually go to <code>http://192.168.4.1</code> in your browser.</p>
</body></html>
)HTML";

String htmlCaptive() { return FPSTR(HTML_CAPTIVE); }
void addCaptiveRoute() {
  server.on("/captive", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200, "text/html", htmlCaptive());
  });
}

static const char SW_JS[] PROGMEM = R"SW(
const CACHE = 'scv-v3';
self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(['/app','/api/items'])));
  self.skipWaiting();
});
self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
  );
  self.clients.claim();
});
self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  if (u.pathname === '/api/items') {
    e.respondWith(
      fetch(e.request).then(r => {
        const cc = r.clone();
        caches.open(CACHE).then(c => c.put(e.request, cc));
        return r;
      }).catch(() => caches.match(e.request))
    );
    return;
  }
  if (u.pathname === '/app') {
    e.respondWith(
      caches.match('/app').then(r => r || fetch(e.request))
    );
    return;
  }
});
)SW";

static const char MANIFEST_JSON[] PROGMEM = R"MANI({
  "name": "Scavenger",
  "short_name": "Scavenger",
  "start_url": "/app",
  "display": "standalone",
  "background_color": "#ffffff",
  "theme_color": "#222222",
  "icons": []
})MANI";

// ------------------ HTTP Routes ------------------

String htmlIndex() { return FPSTR(HTML_INDEX); }
String htmlAdmin() { return FPSTR(HTML_ADMIN); }
String pwaManifest() { return FPSTR(MANIFEST_JSON); }
String pwaServiceWorker() { return FPSTR(SW_JS); }

void setupRoutes() {
  // Root -> app or admin based on current mode
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    if (g_config.mode == MODE_SETUP) req->redirect("/admin");
    else req->redirect("/app");
  });

  // Static pages
  server.on("/app", HTTP_GET, [](AsyncWebServerRequest *req){ req->send(200,"text/html", htmlIndex()); });

  // Protect /admin page after first-time setup
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!adminGuard(req)) return;
    req->send(200,"text/html", htmlAdmin());
  });

  server.on("/manifest.webmanifest", HTTP_GET, [](AsyncWebServerRequest *req){ req->send(200,"application/manifest+json", pwaManifest()); });
  server.on("/sw.js", HTTP_GET, [](AsyncWebServerRequest *req){ req->send(200,"application/javascript", pwaServiceWorker()); });

  // ---- Admin ----
  server.on("/api/admin/status", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!adminGuard(req)) return;
    DynamicJsonDocument d(256);
    d["mode"] = (g_config.mode==MODE_GAME?"game":"setup");
    d["fw_version"] = FW_VERSION;
    d["stored_version"] = g_config.fw_version;
    d["game_ssid"] = g_config.game_ssid;
    sendJSON(req,200,d);
  });

  // First-time password setter (unguarded until set)
  server.on("/api/admin/setup", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      if (g_config.admin_hash.length() > 0) {
        DynamicJsonDocument doc(256); doc["error"]="already_configured";
        sendJSON(req, 400, doc); return;
      }
      String body = getBody(req, data, len);
      DynamicJsonDocument d(512);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String pass = JV_toString(d["pass"], "");
      if (pass.length() < 6) { DynamicJsonDocument e(128); e["error"]="weak_pass"; sendJSON(req,400,e); return; }
      g_config.admin_hash = sha256Hex(pass);
      saveConfig();
      DynamicJsonDocument ok(64); ok["ok"]=true; sendJSON(req,200,ok);
    });

  // Update game_ssid
  server.on("/api/admin/game_ssid", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      if (!adminGuard(req)) return;
      String body = getBody(req, data, len);
      DynamicJsonDocument d(256);
      if (deserializeJson(d, body)) {
        DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return;
      }
      String ssid = JV_toString(d["ssid"], "").substring(0, 31); // limit length
      if (ssid.length()<1) { DynamicJsonDocument e(128); e["error"]="empty_ssid"; sendJSON(req,400,e); return; }
      g_config.game_ssid = ssid;
      saveConfig();
      DynamicJsonDocument ok(64); ok["ok"]=true; ok["game_ssid"]=ssid; sendJSON(req,200,ok);
    });

  // Admin checkpoints: POST save; GET list (for form)
  server.on("/api/admin/checkpoints", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      if (!adminGuard(req)) return;
      String body = getBody(req, data, len);
      DynamicJsonDocument d(16384);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      g_checkpoints.clear();
      for (JsonObject o : d.as<JsonArray>()) {
        Checkpoint c;
        c.id         = JV_toString(o["id"], "");
        if (c.id.length()==0) c.id = newId("C");
        c.name       = sanitizeName(JV_toString(o["name"], ""));
        c.token_text = JV_toString(o["token_text"], "");
        c.token_text.trim();
        if (!saneToken(c.token_text)) continue;
        c.points     = int(o["points"] | 10);
        g_checkpoints.push_back(c);
      }
      saveCheckpoints();
      DynamicJsonDocument ok(64); ok["ok"]=true; ok["count"]= (int)g_checkpoints.size(); sendJSON(req,200,ok);
    });

  server.on("/api/admin/checkpoints", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!adminGuard(req)) return;
    DynamicJsonDocument d(8192);
    JsonArray arr = d.createNestedArray("items");
    for (auto &c : g_checkpoints) {
      JsonObject o = arr.createNestedObject();
      o["id"]=c.id; o["name"]=c.name; o["token_text"]=c.token_text; o["points"]=c.points;
    }
    sendJSON(req,200,d);
  });

  // Switch mode
  server.on("/api/admin/mode", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      if (!adminGuard(req)) return;
      String body = getBody(req, data, len);
      DynamicJsonDocument d(256);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String m = JV_toString(d["mode"], "");
      if (m=="setup") { g_config.mode=MODE_SETUP; saveConfig(); switchAPNow(MODE_SETUP); }
      else if (m=="game"){ g_config.mode=MODE_GAME; saveConfig(); switchAPNow(MODE_GAME); }
      DynamicJsonDocument ok(64); ok["ok"]=true; ok["mode"]=m; sendJSON(req,200,ok);
    });

  // Factory reset
  server.on("/api/admin/factory_reset", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      if (!adminGuard(req)) return;
      String body = getBody(req, data, len);
      DynamicJsonDocument d(256);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      bool wipeAll = bool(d["wipe_all"] | false);
      factoryReset(wipeAll);
      DynamicJsonDocument ok(64); ok["ok"]=true; ok["wipe_all"]=wipeAll; sendJSON(req,200,ok);
      delay(250);
      ESP.restart();
    });

  // ---- Player / Game APIs ----

  // Register team: {team_name, pin}
  server.on("/api/register", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      String body = getBody(req, data, len);
      DynamicJsonDocument d(512);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String name = sanitizeName(JV_toString(d["team_name"], ""));
      String pin  = JV_toString(d["pin"], "");
      if (name.length()<1 || pin.length()<PIN_MINLEN || pin.length()>PIN_MAXLEN) { DynamicJsonDocument e(128); e["error"]="bad_fields"; sendJSON(req,400,e); return; }
      if (findTeamByName(name)) { DynamicJsonDocument e(128); e["error"]="exists"; sendJSON(req,409,e); return; }
      Team t; t.id=newId("T"); t.name=name; t.pin_hash=sha256Hex(pin); t.created_at=millis()/1000;
      updatePointsFromFound(t);
      g_teams.push_back(t);
      saveTeams();
      DynamicJsonDocument ok(256); ok["ok"]=true; ok["team_id"]=t.id; sendJSON(req,200,ok);
    });

  // Login: {team_name, pin}
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      String body = getBody(req, data, len);
      DynamicJsonDocument d(512);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String name = sanitizeName(JV_toString(d["team_name"], ""));
      String pin  = JV_toString(d["pin"], "");
      Team* t = findTeamByName(name);
      if (!t || !consttime_eq(sha256Hex(pin), t->pin_hash)) {
        DynamicJsonDocument e(128); e["error"]="auth"; sendJSON(req,403,e); return;
      }
      DynamicJsonDocument ok(256); ok["ok"]=true; ok["team_id"]=t->id; sendJSON(req,200,ok);
    });

  // Items list (PWA caches this)
  server.on("/api/items", HTTP_GET, [](AsyncWebServerRequest *req){
    DynamicJsonDocument d(8192);
    JsonArray arr = d.createNestedArray("items");
    for (auto &c : g_checkpoints) {
      JsonObject o = arr.createNestedObject();
      o["id"]=c.id; o["name"]=c.name; o["points"]=c.points;
    }
    sendJSON(req,200,d);
  });

  // Items list *for a team* with found/missing flags
  server.on("/api/team/items", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      String body = getBody(req, data, len);
      DynamicJsonDocument d(1024);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String team_id = JV_toString(d["team_id"], "");
      Team* t = findTeamById(team_id);
      if (!t) { DynamicJsonDocument e(128); e["error"]="team_not_found"; sendJSON(req,404,e); return; }

      DynamicJsonDocument out(16384);
      JsonArray arr = out.createNestedArray("items");
      for (auto &c : g_checkpoints) {
        JsonObject o = arr.createNestedObject();
        o["id"]=c.id; o["name"]=c.name; o["points"]=c.points;
        o["found"]= teamFoundHas(*t, c.id);
      }
      sendJSON(req,200,out);
    });

  // Codeword submit (canonical)
  server.on("/api/team/submit_code", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      String body = getBody(req, data, len);
      DynamicJsonDocument d(1024);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String team_id = JV_toString(d["team_id"], "");
      String token   = JV_toString(d["token"], "");
      Team* t = findTeamById(team_id);
      if (!t) { DynamicJsonDocument e(128); e["error"]="team_not_found"; sendJSON(req,404,e); return; }
      token.trim();
      if (token.length()==0) { DynamicJsonDocument e(128); e["error"]="empty_token"; sendJSON(req,400,e); return; }

      // Case-insensitive match convenience
      Checkpoint* c = nullptr;
      for (auto &cc : g_checkpoints) {
        if (consttime_eq(cc.token_text, token)) { c = &cc; break; }
        String a = cc.token_text; a.toLowerCase();
        String b = token; b.toLowerCase();
        if (a == b) { c = &cc; break; }
      }

      if (!c) { DynamicJsonDocument e(128); e["error"]="no_match"; sendJSON(req,404,e); return; }
      if (teamFoundHas(*t, c->id)) {
        DynamicJsonDocument e(256); e["ok"]=true; e["duplicate"]=true; e["points"]=t->points; sendJSON(req,200,e); return;
      }
      teamAddFound(*t, c->id, c->points);
      saveTeams();

      DynamicJsonDocument ok(256);
      ok["ok"]=true; ok["awarded"]=c->points; ok["total"]=t->points; ok["checkpoint_id"]=c->id;
      sendJSON(req,200,ok);
    });

  // Back-compat alias: allow old clients calling /scan_qr to behave the same
  server.on("/api/team/scan_qr", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      // Forward semantics to submit_code
      String body = getBody(req, data, len);
      DynamicJsonDocument d(1024);
      if (deserializeJson(d, body)) { DynamicJsonDocument e(128); e["error"]="bad_json"; sendJSON(req,400,e); return; }
      String team_id = JV_toString(d["team_id"], "");
      String token   = JV_toString(d["token"], "");
      Team* t = findTeamById(team_id);
      if (!t) { DynamicJsonDocument e(128); e["error"]="team_not_found"; sendJSON(req,404,e); return; }
      token.trim();
      if (token.length()==0) { DynamicJsonDocument e(128); e["error"]="empty_token"; sendJSON(req,400,e); return; }

      Checkpoint* c = nullptr;
      for (auto &cc : g_checkpoints) {
        if (consttime_eq(cc.token_text, token)) { c = &cc; break; }
        String a2 = cc.token_text; a2.toLowerCase();
        String b2 = token; b2.toLowerCase();
        if (a2 == b2) { c = &cc; break; }
      }

      if (!c) { DynamicJsonDocument e(128); e["error"]="no_match"; sendJSON(req,404,e); return; }
      if (teamFoundHas(*t, c->id)) {
        DynamicJsonDocument e(256); e["ok"]=true; e["duplicate"]=true; e["points"]=t->points; sendJSON(req,200,e); return;
      }
      teamAddFound(*t, c->id, c->points);
      saveTeams();

      DynamicJsonDocument ok(256);
      ok["ok"]=true; ok["awarded"]=c->points; ok["total"]=t->points; ok["checkpoint_id"]=c->id;
      sendJSON(req,200,ok);
    });

  // Leaderboard
  server.on("/api/leaderboard", HTTP_GET, [](AsyncWebServerRequest *req){
    for (auto &t : g_teams) updatePointsFromFound(t);
    std::vector<Team> v = g_teams;
    std::sort(v.begin(), v.end(), [](const Team&a,const Team&b){
      if (a.points!=b.points) return a.points>b.points;
      return a.created_at<b.created_at;
    });
    DynamicJsonDocument d(8192);
    JsonArray arr = d.createNestedArray("teams");
    int count = 0;
    for (auto &t: v) {
      JsonObject o = arr.createNestedObject();
      o["name"]=t.name; o["points"]=t.points; o["found"]= (int)t.found.size();
      if (++count>=LEADERBOARD_SIZE) break;
    }
    sendJSON(req,200,d);
  });
}

// ------------------ Mode management ------------------

void enterSetupMode() { startSetupAP(); }
void enterGameMode()  { startGameAP();  }

// ------------------ Factory reset ------------------

void factoryReset(bool wipeAll) {
  if (wipeAll) {
    LittleFS.format();
    g_config.admin_hash = "";
    g_config.mode = MODE_SETUP;
    g_config.fw_version = FW_VERSION;
    saveConfig();
  } else {
    g_config.admin_hash = "";
    g_config.mode = MODE_SETUP;
    saveConfig();
  }
}

// ------------------ Setup / Loop ------------------

void mountFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
  } else {
    Serial.println("[FS] LittleFS mounted");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  mountFS();
  loadAll();

  // If no admin password yet, force SETUP mode and persist it
  if (g_config.admin_hash.length() == 0) {
    g_config.mode = MODE_SETUP;
    saveConfig();
  }

  if (g_config.mode == MODE_SETUP) enterSetupMode();
  else enterGameMode();

  setupRoutes();
  addCaptiveRoute();
  registerCaptiveHandlersOnce(server);

  server.begin();
  Serial.println("[HTTP] Server started");
  Serial.println("Ready.");
}

void loop() {
  dnsServer.processNextRequest();
  vTaskDelay(pdMS_TO_TICKS(10));  // portable and clear
}
