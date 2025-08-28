/*
   D1 mini (ESP8266) + SCD30 + 128x64 OLED (U8g2 / SSD1309 or SSD1306)
   --------------------------------------------------------------------
   - WiFiManager captive portal for Wi-Fi setup (fallback AP "D1Mini-Setup", pass "config123")
   - Web UI (dark mode) with:
        * UDP target IP + port (telemetry)
        * UDP brightness listen port
        * Live brightness slider (0..100, 0 = OFF)
   - Sends SCD30 data via UDP (labels: T,H,C)
   - OLED shows a large CO2 readout

   Flash-wear safe:
   - Brightness is RAM-only and resets to 0 (OFF) on each boot.
*/

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <SparkFun_SCD30_Arduino_Library.h>
#include <U8g2lib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ===================== Display =====================
// Choose the constructor matching your display:
/* SSD1309 128x64 */
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
/* If you have SSD1306, comment the SSD1309 line and uncomment one of these: */
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_VCOMH0_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ===================== SCD30 =====================
SCD30 scd30;

// ===================== UDP / Net =====================
WiFiUDP udpTx;           // telemetry sender
WiFiUDP udpRx;           // brightness listener
ESP8266WebServer server(80);

// ===================== Config storage (flash) =====================
// Only persistent items remain here (no brightness)
struct AppConfig {
  char udpTargetIP[16];   // "192.168.1.123"
  uint16_t udpTargetPort; // telemetry target port
  uint16_t udpListenPort; // brightness control port
};

AppConfig cfg = {
  "192.168.1.200",
  9050,
  2807
};

const char* CONFIG_PATH = "/config.json";

// ===================== Runtime state (RAM only) =====================
uint8_t currentBrightness = 0;  // 0..100; defaults to 0 on boot (display off)

// ===================== Helpers =====================
static void saveConfig(const AppConfig& c) {
  if (!LittleFS.begin()) LittleFS.begin();
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return;
  StaticJsonDocument<192> doc;
  doc["udpTargetIP"]   = c.udpTargetIP;
  doc["udpTargetPort"] = c.udpTargetPort;
  doc["udpListenPort"] = c.udpListenPort;
  serializeJson(doc, f);
  f.close();
}

static void loadConfig(AppConfig& c) {
  if (!LittleFS.begin()) { LittleFS.begin(); }
  if (!LittleFS.exists(CONFIG_PATH)) { saveConfig(c); return; }
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  strlcpy(c.udpTargetIP, doc["udpTargetIP"] | "192.168.1.200", sizeof(c.udpTargetIP));
  c.udpTargetPort = doc["udpTargetPort"] | 9050;
  c.udpListenPort = doc["udpListenPort"] | 2807;
}

static uint8_t clamp255(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }

static void applyBrightness(uint8_t pct) {
  // 0..100 map to 0..255; at 0, turn display off via power-save
  if (pct == 0) {
    u8g2.setPowerSave(1);
  } else {
    u8g2.setPowerSave(0);
    uint8_t contrast = clamp255((int)(pct * 255 / 100));
    u8g2.setContrast(contrast);
  }
}

// Safely push a frame with yields + one retry
static void safeSendBuffer() {
  yield();
  u8g2.sendBuffer();
  // extra small yield; if a glitch was observed, push once more
  yield();
  // Heuristic: send twice to correct any partial push (cheap for 128x64)
  u8g2.sendBuffer();
  yield();
}

// ===================== WiFi provisioning (dark portal) =====================
const char* AP_SSID = "D1Mini-Setup";
const char* AP_PASS = "config123";

const char* WM_DARK_CSS =
  "<style>"
  "html,body{background:#0b0d10;color:#e6e6e6;font-family:system-ui,Segoe UI,Roboto,Arial}"
  "a{color:#8ab4f8}"
  "input,button,select{background:#111418;color:#e6e6e6;border:1px solid #2a2f36;border-radius:8px;padding:10px}"
  "button{cursor:pointer}"
  ".wrap, .container, .q, .msg{background:#0b0d10!important}"
  ".q label{color:#cfcfcf}"
  ".btn, input[type=submit]{background:#1f2933;border:1px solid #334155}"
  ".btn:hover, input[type=submit]:hover{background:#2b3845}"
  ".center, .title{color:#e6e6e6}"
  "hr{border-color:#2a2f36}"
  "</style>";

static void startWifiWithPortal() {
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setTitle("D1 mini CO\u2082 (Dark)");
  wm.setCustomHeadElement(WM_DARK_CSS);
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect(AP_SSID, AP_PASS);
  if (!ok) {
    wm.startConfigPortal(AP_SSID, AP_PASS);
  }
}

// ===================== Web server (dark) =====================
static String htmlHeader() {
  String s =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>D1 mini CO2 Monitor</title>"
    "<style>"
    ":root{--bg:#0b0d10;--bg2:#111418;--fg:#e6e6e6;--muted:#b8c0cc;--line:#2a2f36;--accent:#8ab4f8;--btn:#1f2933;--btnh:#2b3845}"
    "html,body{background:var(--bg);color:var(--fg);font-family:system-ui,Segoe UI,Roboto,Arial;margin:0}"
    "main{max-width:720px;margin:24px auto;padding:0 16px}"
    "h1,h2{font-weight:600;margin:0 0 12px}"
    "a{color:var(--accent)}"
    ".card{background:var(--bg2);border:1px solid var(--line);border-radius:12px;padding:16px;margin:16px 0}"
    "label{display:block;margin:12px 0 6px;color:var(--muted)}"
    "input[type=text],input[type=number]{padding:10px;width:100%;box-sizing:border-box;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:10px}"
    "button{padding:10px 14px;border-radius:10px;border:1px solid #334155;background:var(--btn);color:var(--fg);cursor:pointer}"
    "button:hover{background:var(--btnh)}"
    "code{background:#0f1318;padding:2px 6px;border-radius:6px}"
    ".row{display:flex;gap:12px;flex-wrap:wrap}"
    ".row .col{flex:1 1 220px}"
    ".slider{width:100%}"
    ".value{font-variant-numeric:tabular-nums}"
    "</style></head><body><main>";
  return s;
}

// Live brightness API (query: v=0..100)
static void handleApiBrightness() {
  if (!server.hasArg("v")) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing v\"}"); return; }
  int v = server.arg("v").toInt();
  if (v < 0) v = 0; if (v > 100) v = 100;
  currentBrightness = (uint8_t)v;
  applyBrightness(currentBrightness);
  String out = String("{\"ok\":true,\"brightness\":") + String(currentBrightness) + "}";
  server.send(200, "application/json", out);
}

static void handleRoot() {
  String s = htmlHeader();
  s += "<h2>D1 mini CO₂ Monitor</h2>";

  // status card
  s += "<div class='card'><b>Status</b><br/>";
  s += "IP: <code>" + WiFi.localIP().toString() + "</code><br/>";
  s += "UDP Target: <code>" + String(cfg.udpTargetIP) + ":" + String(cfg.udpTargetPort) + "</code><br/>";
  s += "Brightness port: <code>" + String(cfg.udpListenPort) + "</code><br/>";
  s += "Current brightness: <code class='value' id='bval'>" + String(currentBrightness) + "%</code></div>";

  // brightness slider
  s += "<div class='card'><b>Brightness</b>"
       "<input id='slider' class='slider' type='range' min='0' max='100' step='1' value='" + String(currentBrightness) + "'>"
       "<div style='margin-top:6px'>0 = off, 100 = max. (Not saved; resets to 0 on reboot.)</div>"
       "</div>";

  // settings form
  s += "<div class='card'><b>Settings</b>"
       "<form method='POST' action='/save'><div class='row'>"
       "<div class='col'><label>UDP target IP</label><input name='ip' value='" + String(cfg.udpTargetIP) + "'/></div>"
       "<div class='col'><label>UDP target port</label><input type='number' min='1' max='65535' name='port' value='" + String(cfg.udpTargetPort) + "'/></div>"
       "<div class='col'><label>UDP brightness listen port</label><input type='number' min='1' max='65535' name='listen' value='" + String(cfg.udpListenPort) + "'/></div>"
       "</div><button type='submit'>Save</button></form></div>";

  // footer + JS
  s += "<p style='color:#97a3b6'>Tip: You can also send UDP packets with a single integer <code>0..100</code> to the brightness port to change brightness live.</p>";

  s += "<script>"
       "const bval=document.getElementById('bval');"
       "const sld=document.getElementById('slider');"
       "let t=null;"
       "function push(v){fetch('/api/brightness?v='+v).then(r=>r.json()).then(j=>{bval.textContent=j.brightness+'%';}).catch(()=>{});}"
       "sld.addEventListener('input',e=>{bval.textContent=e.target.value+'%'; if(t)clearTimeout(t); t=setTimeout(()=>push(e.target.value),150);});"
       "</script>";

  s += "</main></body></html>";
  server.send(200, "text/html", s);
}

static void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Use POST"); return; }
  String ip   = server.arg("ip");
  int port    = server.arg("port").toInt();
  int listen  = server.arg("listen").toInt();

  if (ip.length() < 7 || port <= 0 || port > 65535 || listen <= 0 || listen > 65535) {
    server.send(400, "text/plain", "Invalid values");
    return;
  }

  strlcpy(cfg.udpTargetIP, ip.c_str(), sizeof(cfg.udpTargetIP));
  cfg.udpTargetPort = (uint16_t)port;
  cfg.udpListenPort = (uint16_t)listen;
  saveConfig(cfg);

  // rebind UDP listener if port changed
  udpRx.stop();
  udpRx.begin(cfg.udpListenPort);
  applyBrightness(currentBrightness);

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved");
}

// ===================== Telemetry =====================
static void sendTelemetry(float co2, float tempC, float rh) {
  IPAddress ip;
  if (!ip.fromString(cfg.udpTargetIP)) return;
  char buf[16];

  // Temperature
  //udpTx.beginPacket(ip, cfg.udpTargetPort);
  //udpTx.write("T");
  //dtostrf(tempC, 0, 2, buf);
  //udpTx.write((const uint8_t*)buf, strlen(buf));
  //udpTx.endPacket();

  // Humidity
  //udpTx.beginPacket(ip, cfg.udpTargetPort);
  //udpTx.write("H");
  //dtostrf(rh, 0, 2, buf);
  //udpTx.write((const uint8_t*)buf, strlen(buf));
  //udpTx.endPacket();

  // CO2
  udpTx.beginPacket(ip, cfg.udpTargetPort);
  udpTx.write("C");
  dtostrf(co2, 0, 0, buf);
  udpTx.write((const uint8_t*)buf, strlen(buf));
  udpTx.endPacket();
}

// ===================== UDP brightness listener =====================
static void pollUdpBrightness() {
  int packetSize = udpRx.parsePacket();
  if (packetSize <= 0) return;

  char msg[32];
  int n = udpRx.read(msg, sizeof(msg) - 1);
  if (n <= 0) return;
  msg[n] = '\0';  // ensure termination for ASCII parsing

  int value = -1;

  // --- Try ASCII decimal first: handles "0", "100", "42\n", " 7 "
  int i = 0;
  while (i < n && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '\r' || msg[i] == '\n')) i++;  // trim left
  bool hasDigits = false;
  long acc = 0;
  while (i < n && msg[i] >= '0' && msg[i] <= '9') { hasDigits = true; acc = acc * 10 + (msg[i] - '0'); i++; }
  while (i < n && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '\r' || msg[i] == '\n')) i++;  // trim right

  if (hasDigits && i == n) {
    value = (int)acc;                       // parsed ASCII number
  } else if (n == 1) {
    value = (uint8_t)msg[0];                // fall back to raw single-byte (0..100)
  } else {
    return;                                  // unrecognized payload
  }

  if (value < 0)   value = 0;
  if (value > 100) value = 100;

  if (currentBrightness != (uint8_t)value) {
    currentBrightness = (uint8_t)value;
    applyBrightness(currentBrightness);      // 0 -> OFF via power-save, >0 -> set contrast
  }
}


// ===================== Display rendering =====================
static void drawSplash() {
  // Hard-wake + draw once
  u8g2.setPowerSave(0);                             // ensure panel ON
  u8g2.setContrast(clamp255((int)(60 * 255 / 100))); // ~60% just for splash
  delay(30);                                        // let charge pump settle

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tf);
  u8g2.drawStr(0, 14, "CO2 Monitor");
  u8g2.drawStr(0, 30, "WiFi connecting...");
  u8g2.sendBuffer();
  delay(1000);                                       // keep visible briefly
}



static void drawReadings(float co2, float tempC, float rh) {
  yield();
  u8g2.clearBuffer();

  // Big CO2 number
  u8g2.setFont(u8g2_font_logisoso50_tn);
  char co2buf[12];
  dtostrf(co2, 0, 0, co2buf);
  int16_t w = u8g2.getStrWidth(co2buf);
  int16_t x = (u8g2.getDisplayWidth() - w) / 2;
  u8g2.drawStr(x, 54, co2buf);

  // (You commented out labels; keeping that choice)
  safeSendBuffer();
  yield();
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  delay(100);

  // Filesystem + config
  loadConfig(cfg);

  // I2C
  Wire.begin();
  Wire.setClock(100000);            // 100 kHz for robustness (try 400 kHz later if you wish)

  // Display
  u8g2.begin();
  drawSplash();                 // show while panel is ON

  currentBrightness = 0;        // now apply your default OFF
  applyBrightness(currentBrightness);


  // WiFi with (dark) portal
  startWifiWithPortal();

  // Web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/brightness", HTTP_GET, handleApiBrightness);
  server.begin();

  // UDP
  udpTx.begin(0);                   // ephemeral local port for TX
  udpRx.begin(cfg.udpListenPort);   // listen for brightness

  // SCD30
  if (!scd30.begin()) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_tf);
    u8g2.drawStr(0, 14, "SCD30 not found!");
    safeSendBuffer();
  } else {
    scd30.setMeasurementInterval(2);
    scd30.setAutoSelfCalibration(true);
  }
}

uint32_t lastPoll = 0;
uint32_t lastRefresh = 0;           // periodic redraw watchdog
float lastCO2 = NAN, lastT = NAN, lastRH = NAN;

void loop() {
  server.handleClient();
  pollUdpBrightness();

  // Poll SCD30 when data ready
  if (millis() - lastPoll >= 200) {
    lastPoll = millis();
    if (scd30.dataAvailable()) {
      lastCO2 = scd30.getCO2();
      lastT   = scd30.getTemperature();
      lastRH  = scd30.getHumidity();
      drawReadings(lastCO2, lastT, lastRH);
      sendTelemetry(lastCO2, lastT, lastRH);
    }
  }

  // Periodic redraw (2 Hz) to “heal” any tor
 if (millis() - lastRefresh >= 500) {
    lastRefresh = millis();
    drawReadings(lastCO2, lastT, lastRH);
  }
}