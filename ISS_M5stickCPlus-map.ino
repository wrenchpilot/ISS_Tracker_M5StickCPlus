/*
  ISS Tracker — M5StickC Plus (v1.14 full)
  ----------------------------------------
  - Serves static UI from LittleFS: / (index.html), /setup.html, /app.js, /setup.js, /styles.css
  - JSON endpoints:
      /iss.json      -> {haveFix, iss:{lat,lon,vel,dir,age_ms,dist_km}, home:{lat,lon}}
      /config.json   -> {wifi:{ssid,ip}, home:{lat,lon}, loc_token:"..."}
      /scan.json     -> {nets:[{ssid,rssi,locked}]}
      /track.json    -> {points:[{t,lat,lon}, ...]}  // last ~1 hour
      /loc           -> set home (GET ?lat&lon or POST {lat,lon}, optional Bearer token)
      /savehome      -> legacy form (POST lat/lon) -> redirects
      /save          -> save WiFi SSID/pass then reboot
      /forget        -> forget WiFi then reboot

  - Live polling of ISS every 5s from open-notify API (simple demo)
  - Persists track to LittleFS (newline-delimited JSON), keeps RAM ring buffer
  - Draggable Home marker handled by frontend; this exposes /loc to persist

  Board:  M5Stick-C Plus
  Libs:   M5StickCPlus, ArduinoJson, WiFi, HTTPClient, WebServer, ESPmDNS,
          Preferences, DNSServer, LittleFS
*/

#include <M5StickCPlus.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESP.h>
#include <math.h>
#include <LittleFS.h>

#include "user_settings.h"  // WIFI_SSID, WIFI_PASSWORD, HOME_LAT, HOME_LON, LOC_TOKEN (may be "")

// =========================== CONFIG ===========================
constexpr double RADIUS_KM = 800.0;
constexpr double HYST_KM = 200.0;  // re-arm once outside RADIUS_KM + HYST_KM
constexpr uint32_t FETCH_INTERVAL_MS = 5000;

constexpr int BUZZER_PIN = 26;
constexpr uint8_t BUZZER_RES_BITS = 10;
constexpr uint32_t BUZZER_INIT_HZ = 4000;

constexpr uint16_t WIFI_BEEP_HZ = 1400;
constexpr uint16_t WIFI_BEEP_MS = 90;
constexpr uint16_t ISS_BEEP_HZ = 2300;  // proximity beep tone
constexpr uint16_t ISS_BEEP_MS = 120;   // each beep ON duration

// Continuous-beep cadence (closer = faster)
constexpr uint16_t MIN_BEEP_PERIOD_MS = 220;   // at 0 km
constexpr uint16_t MAX_BEEP_PERIOD_MS = 2800;  // at RADIUS_KM

// Onboard LED (M5StickC Plus): GPIO 10, active-low
constexpr int LED_PIN = 10;
constexpr bool LED_ACTIVE_LOW = true;

// Wi-Fi portal behavior
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_STALE_REVERT_MS = 45000;

static const char* MDNS_NAME = "iss";

// ISS API (demo)
static const char* ISS_URL = "http://api.open-notify.org/iss-now.json";

// ============ TRACK PERSISTENCE (RAM + LittleFS) ============
struct TrackPt {
  uint32_t t;
  float lat;
  float lon;
};                                          // t = millis() of sample
static const uint16_t TRACK_RAM_MAX = 900;  // ~75 min @5s
static TrackPt trackBuf[TRACK_RAM_MAX];
static uint16_t trackCount = 0;

static const char* TRACK_FILE = "/track.log";  // newline-delimited JSON
static const uint32_t TRACK_FLUSH_MS = 15000;  // flush latest point every 15s
static uint32_t lastTrackFlush = 0;

// =========================== STATE ===========================
uint32_t lastFetch = 0;
bool soundEnabled = true;

double issLat = 0.0, issLon = 0.0;
double prevLat = NAN, prevLon = NAN;
uint32_t prevSampleMs = 0;

double velKmh = NAN;     // latest computed velocity (km/h)
double velKmhEma = NAN;  // smoothed velocity
bool velValid = false;
String velDir8 = "";  // 8-point direction label for velocity

bool haveFix = false;       // true once we have at least one ISS sample
double homeLat = HOME_LAT;  // dynamic home (from /loc or NVS)
double homeLon = HOME_LON;

bool wasClose = false;  // hysteresis latch

// LED + beep scheduler
bool ledState = false;
bool ledBlinking = false;
uint32_t ledLastToggle = 0;

bool contBeepActive = false;
uint32_t nextBeepAtMs = 0;
uint32_t beepOffAtMs = 0;

// Wi-Fi state
enum class WifiState { STA_OK,
                       PORTAL };
WifiState wifiState = WifiState::PORTAL;
uint32_t lastStaOkMs = 0;

// Services
WebServer server(80);
Preferences prefs;
DNSServer dns;

// =================== HELPERS: math ===================
static inline double toRad(double deg) {
  return deg * M_PI / 180.0;
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0;  // km
  const double dLat = toRad(lat2 - lat1);
  const double dLon = toRad(lon2 - lon1);
  const double a = sin(dLat * 0.5) * sin(dLat * 0.5)
                   + cos(toRad(lat1)) * cos(toRad(lat2))
                       * sin(dLon * 0.5) * sin(dLon * 0.5);
  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

// Initial bearing (deg) from (lat1,lon1) -> (lat2,lon2)
double initialBearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = toRad(lat1), phi2 = toRad(lat2);
  double dLon = toRad(lon2 - lon1);
  double y = sin(dLon) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
  double th = atan2(y, x);         // -pi..pi
  double deg = th * 180.0 / M_PI;  // -180..180
  if (deg < 0) deg += 360.0;
  return deg;
}

// 8-point compass label
String bearingTo8(double deg) {
  static const char* dir8[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  int idx = (int)floor((deg + 22.5) / 45.0) % 8;
  return String(dir8[idx]);
}

// =================== HELPERS: LED / BUZZER ===================
inline void ledWrite(bool on) {
  digitalWrite(LED_PIN, (LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW)));
}
inline void ledOff() {
  ledState = false;
  ledWrite(false);
}
inline void ledOn() {
  ledState = true;
  ledWrite(true);
}

static inline void buzzerAttachIfNeeded() {
  ledcAttach(BUZZER_PIN, BUZZER_INIT_HZ, BUZZER_RES_BITS);
}
static inline void buzzerDetachIfNeeded() {
  ledcDetach(BUZZER_PIN);
}
static inline void toneStart(uint16_t f) {
  ledcWriteTone(BUZZER_PIN, f);
}
static inline void toneStop() {
  ledcWriteTone(BUZZER_PIN, 0);
}

void beep(uint16_t freqHz, uint16_t ms) {
  if (!soundEnabled) return;
  buzzerAttachIfNeeded();
  toneStart(freqHz);
  delay(ms);
  toneStop();
  buzzerDetachIfNeeded();
}

// Continuous proximity beep scheduler
uint32_t periodForDistance(double distKm) {
  if (distKm < 0) distKm = 0;
  if (distKm > RADIUS_KM) distKm = RADIUS_KM;
  double t = distKm / RADIUS_KM;  // 0 near, 1 at edge
  double period = MIN_BEEP_PERIOD_MS + t * (MAX_BEEP_PERIOD_MS - MIN_BEEP_PERIOD_MS);
  if (period < MIN_BEEP_PERIOD_MS) period = MIN_BEEP_PERIOD_MS;
  if (period > MAX_BEEP_PERIOD_MS) period = MAX_BEEP_PERIOD_MS;
  return (uint32_t)period;
}
void startContinuousBeepNow() {
  if (!soundEnabled) return;
  buzzerAttachIfNeeded();
  toneStart(ISS_BEEP_HZ);
  contBeepActive = true;
}
void stopContinuousBeepNow() {
  if (!contBeepActive) return;
  toneStop();
  buzzerDetachIfNeeded();
  contBeepActive = false;
}

// =================== HELPERS: M5 UI ===================
void drawHeader() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("ISS Tracker", 4, 2);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawString(soundEnabled ? "A:Sound ON" : "A:Sound OFF", 175, 4);
  M5.Lcd.drawString("B:Refresh", 175, 18);
}

void drawValues(double lat, double lon, double distKm, bool showVel, double vKmh, const String& dir8) {
  int y = 28;
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.drawString("Lat:", 4, y);
  M5.Lcd.drawFloat(lat, 4, 65, y);
  y += 22;
  M5.Lcd.drawString("Lon:", 4, y);
  M5.Lcd.drawFloat(lon, 4, 65, y);
  y += 22;

  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString("Vol:", 4, y);
  if (showVel) {
    M5.Lcd.drawFloat(vKmh, 1, 65, y);
    M5.Lcd.drawString("km/h", 155, y);
    if (dir8.length()) M5.Lcd.drawString(dir8.c_str(), 210, y);
  } else {
    M5.Lcd.drawString("---", 65, y);
  }
  y += 22;

  M5.Lcd.setTextColor(CYAN, BLACK);
  M5.Lcd.drawString("Dist:", 4, y);
  M5.Lcd.drawFloat(distKm, 1, 65, y);
  M5.Lcd.drawString("km", 155, y);
}

void drawProximityBar(double distKm) {
  const int barX = 4;
  const int barY = 116;
  const int barW = M5.Lcd.width() - 8;
  const int barH = 18;

  String status;
  uint16_t colFill;
  if (distKm <= RADIUS_KM) {
    status = "Close";
    colFill = GREEN;
  } else if (distKm <= 1200.0) {
    status = "Near";
    colFill = YELLOW;
  } else if (distKm <= 1600.0) {
    status = "Mid";
    colFill = ORANGE;
  } else if (distKm <= 2000.0) {
    status = "Far";
    colFill = RED;
  } else {
    status = "Distant";
    colFill = RED;
  }

  M5.Lcd.drawRect(barX, barY, barW, barH, WHITE);

  double maxKm = 2000.0;
  int fillw = (distKm > 1600.0) ? (barW - 2)
                                : (int)((maxKm - max(0.0, distKm)) / maxKm * (barW - 2));
  M5.Lcd.fillRect(barX + 1, barY + 1, fillw, barH - 2, colFill);

  for (int km = 500; km < 2000; km += 500) {
    int tx = barX + 1 + (int)((maxKm - km) / maxKm * (barW - 2));
    M5.Lcd.drawLine(tx, barY, tx, barY + barH, DARKGREY);
  }

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  int statusY = barY + (barH - 8) / 2;
  M5.Lcd.drawCentreString(status, M5.Lcd.width() / 2, statusY - 4, 1);
}

void drawMiniMap(double curLat, double curLon) {
  const bool SHOW_MINIMAP = true;
  if (!SHOW_MINIMAP) return;
  const int mapX = 170, mapY = 28, mapW = 66, mapH = 44;

  M5.Lcd.fillRect(mapX, mapY, mapW, mapH, TFT_NAVY);
  M5.Lcd.drawRect(mapX, mapY, mapW, mapH, DARKGREY);

  for (int lon = -150; lon <= 180; lon += 30) {
    int x = mapX + (int)((lon + 180.0) / 360.0 * (mapW - 1));
    M5.Lcd.drawLine(x, mapY, x, mapY + mapH - 1, DARKGREY);
  }
  for (int lat = -60; lat <= 60; lat += 15) {
    int y = mapY + (int)((90.0 - (double)lat) / 180.0 * (mapH - 1));
    M5.Lcd.drawLine(mapX, y, mapX + mapW - 1, y, DARKGREY);
  }

  auto pxX = [&](double lon) -> int {
    double L = fmod((lon + 540.0), 360.0) - 180.0;
    return mapX + (int)((L + 180.0) / 360.0 * (mapW - 1));
  };
  auto pxY = [&](double lat) -> int {
    double c = lat;
    if (c > 90) c = 90;
    if (c < -90) c = -90;
    return mapY + (int)((90.0 - c) / 180.0 * (mapH - 1));
  };

  int hx = pxX(homeLon), hy = pxY(homeLat);
  M5.Lcd.fillCircle(hx, hy, 2, GREEN);
  M5.Lcd.drawPixel(hx, hy, WHITE);

  int ix = pxX(curLon), iy = pxY(curLat);
  M5.Lcd.fillCircle(ix, iy, 2, YELLOW);
  M5.Lcd.drawPixel(ix, iy, WHITE);

  M5.Lcd.setTextColor(WHITE, TFT_NAVY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawString("MAP", mapX + 2, mapY + 2);
}

// =================== PERSISTENCE (NVS) ===================
void loadHomeFromNVS() {
  prefs.begin("iss", true);
  double lat = prefs.getDouble("homeLat", NAN);
  double lon = prefs.getDouble("homeLon", NAN);
  prefs.end();
  if (!isnan(lat) && !isnan(lon)) {
    homeLat = lat;
    homeLon = lon;
  }
}
void saveHomeToNVS(double lat, double lon) {
  prefs.begin("iss", false);
  prefs.putDouble("homeLat", lat);
  prefs.putDouble("homeLon", lon);
  prefs.end();
}

// Wi-Fi creds
bool loadWifiCreds(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();
  if (s.length()) {
    ssid = s;
    pass = p;
    return true;
  }
  return false;
}
void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}
void forgetWifiCreds() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// Auth helper for /loc
bool tokenOk(const String& bearerOrQuery) {
  if (LOC_TOKEN[0] == 0) return true;
  return bearerOrQuery == LOC_TOKEN;
}

// =================== ISS Networking ===================
bool fetchISS(double& outLat, double& outLon) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, ISS_URL)) return false;

  bool ok = false;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      outLat = atof(doc["iss_position"]["latitude"] | "0");
      outLon = atof(doc["iss_position"]["longitude"] | "0");
      ok = true;
    }
  }
  http.end();
  return ok;
}

// =================== AP/Portal helpers ===================
String apSSID() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "ISS-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}
void drawPortalBanner(IPAddress apIp) {
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  M5.Lcd.fillRect(4, 150, 236, 20, BLACK);
  String s = "Setup AP: " + apSSID() + "  " + apIp.toString();
  M5.Lcd.drawString(s, 4, 150);
}

// =================== LittleFS Static ===================
String contentTypeFor(const String& path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css")) return "text/css; charset=utf-8";
  if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
  if (path.endsWith(".json")) return "application/json; charset=utf-8";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg")) return "image/svg+xml";
  return "text/plain; charset=utf-8";
}
bool serveStaticFile(String path) {
  String gz = path + ".gz";
  if (LittleFS.exists(gz)) {
    File f = LittleFS.open(gz, "r");
    if (!f) return false;
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "public, max-age=604800, immutable");
    server.streamFile(f, contentTypeFor(path));
    f.close();
    return true;
  }
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  server.sendHeader("Cache-Control", "public, max-age=604800, immutable");
  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

// =================== TRACK: RAM + LittleFS ===================
void trackPush(uint32_t t, double lat, double lon) {
  if (trackCount < TRACK_RAM_MAX) {
    trackBuf[trackCount++] = { t, (float)lat, (float)lon };
  } else {
    // ring: shift left 1 (small buffer, simple)
    memmove(trackBuf, trackBuf + 1, sizeof(TrackPt) * (TRACK_RAM_MAX - 1));
    trackBuf[TRACK_RAM_MAX - 1] = { t, (float)lat, (float)lon };
  }
}

void trackFlushIfDue() {
  uint32_t now = millis();
  if (now - lastTrackFlush < TRACK_FLUSH_MS) return;
  lastTrackFlush = now;
  if (trackCount == 0) return;

  // append the last point only
  TrackPt& p = trackBuf[trackCount - 1];
  File f = LittleFS.open(TRACK_FILE, FILE_APPEND);
  if (!f) return;
  StaticJsonDocument<96> doc;
  doc["t"] = p.t;
  doc["lat"] = p.lat;
  doc["lon"] = p.lon;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
}

void handleTrackJson() {
  // Collect last ~1 hour by uptime-ms window from file + RAM
  const uint32_t NOW = millis();
  const uint32_t WINDOW = 3600UL * 1000UL;  // 1 hour

  // We’ll keep a bounded list
  const size_t MAX_PTS = 2000;
  static TrackPt tmp[MAX_PTS];
  size_t n = 0;

  if (LittleFS.exists(TRACK_FILE)) {
    File f = LittleFS.open(TRACK_FILE, FILE_READ);
    if (f) {
      while (f.available() && n < MAX_PTS) {
        String line = f.readStringUntil('\n');
        StaticJsonDocument<96> d;
        if (deserializeJson(d, line) == DeserializationError::Ok) {
          uint32_t t = d["t"] | 0;
          double la = d["lat"] | NAN, lo = d["lon"] | NAN;
          if (!isnan(la) && !isnan(lo)) {
            if (NOW - t <= WINDOW) {
              tmp[n++] = { t, (float)la, (float)lo };
            }
          }
        }
      }
      f.close();
    }
  }

  // Merge RAM tail (already ~last hour by size)
  uint16_t start = (trackCount > MAX_PTS ? trackCount - MAX_PTS : 0);
  for (uint16_t i = start; i < trackCount && n < MAX_PTS; ++i) {
    if (NOW - trackBuf[i].t <= WINDOW) {
      tmp[n++] = trackBuf[i];
    }
  }

  // Emit JSON
  String out;
  {
    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.createNestedArray("points");
    size_t begin = (n > MAX_PTS ? n - MAX_PTS : 0);
    for (size_t i = begin; i < n; ++i) {
      JsonObject o = arr.createNestedObject();
      o["t"] = tmp[i].t;
      o["lat"] = tmp[i].lat;
      o["lon"] = tmp[i].lon;
    }
    serializeJson(doc, out);
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// =================== JSON Endpoints ===================
void handleIssJson() {
  DynamicJsonDocument doc(256);
  doc["haveFix"] = haveFix;
  if (haveFix) {
    doc["iss"]["lat"] = issLat;
    doc["iss"]["lon"] = issLon;
    doc["iss"]["vel"] = velKmhEma;
    doc["iss"]["dir"] = velDir8;
    doc["iss"]["age_ms"] = (uint32_t)(millis() - prevSampleMs);
    doc["iss"]["dist_km"] = greatCircleKm(homeLat, homeLon, issLat, issLon);
  }
  doc["home"]["lat"] = homeLat;
  doc["home"]["lon"] = homeLon;
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleConfigJson() {
  DynamicJsonDocument doc(192);
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = WiFi.SSID();
  wifi["ip"] = WiFi.localIP().toString();
  JsonObject home = doc.createNestedObject("home");
  home["lat"] = homeLat;
  home["lon"] = homeLon;
  // Expose token to client so it can POST /loc with Bearer (optional)
  doc["loc_token"] = String(LOC_TOKEN);
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleScanJson() {
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, true);
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("nets");
  for (int i = 0; i < n; ++i) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["locked"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// Actions
void handleSave() {
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if (!ssid.length()) {
    server.send(400, "text/plain", "SSID required");
    return;
  }
  saveWifiCreds(ssid, pass);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8",
              "<meta http-equiv='refresh' content='3;url=/'/>Saved. Rebooting…");
  delay(750);
  ESP.restart();
}
void handleForget() {
  forgetWifiCreds();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8",
              "<meta http-equiv='refresh' content='3;url=/setup.html'/>Forgotten. Rebooting…");
  delay(750);
  ESP.restart();
}

// /loc endpoint — GET ?lat&lon&token=... or POST JSON {lat,lon,token?}
void handleLoc() {
  String tok = "";
  if (server.hasHeader("Authorization")) {
    String h = server.header("Authorization");
    if (h.startsWith("Bearer ")) tok = h.substring(7);
  }
  if (tok == "" && server.hasArg("token")) tok = server.arg("token");
  if (!tokenOk(tok)) {
    server.send(401, "application/json", "{\"err\":\"unauthorized\"}");
    return;
  }

  double lat = NAN, lon = NAN;
  if (server.method() == HTTP_GET) {
    if (server.hasArg("lat")) lat = server.arg("lat").toDouble();
    if (server.hasArg("lon")) lon = server.arg("lon").toDouble();
  } else if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      lat = doc["lat"] | NAN;
      lon = doc["lon"] | NAN;
      if (tok == "" && doc.containsKey("token")) tok = (const char*)doc["token"];
      if (!tokenOk(tok)) {
        server.send(401, "application/json", "{\"err\":\"unauthorized\"}");
        return;
      }
    }
  }
  if (isnan(lat) || isnan(lon)) {
    server.send(400, "application/json", "{\"err\":\"lat/lon required\"}");
    return;
  }

  homeLat = lat;
  homeLon = lon;
  saveHomeToNVS(lat, lon);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveHome() {
  double lat = NAN, lon = NAN;
  if (server.hasArg("lat")) lat = server.arg("lat").toDouble();
  if (server.hasArg("lon")) lon = server.arg("lon").toDouble();
  if (isnan(lat) || isnan(lon)) {
    server.send(400, "text/plain; charset=utf-8", "lat/lon required");
    return;
  }
  homeLat = lat;
  homeLon = lon;
  saveHomeToNVS(lat, lon);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", "<meta http-equiv='refresh' content='1;url=/'/>Saved.");
}

// =================== Static routes ===================
void handleIndex() {
  if (!serveStaticFile("/index.html")) server.send(404, "text/plain", "index.html not found");
}
void handleSetupHtml() {
  if (!serveStaticFile("/setup.html")) server.send(404, "text/plain", "setup.html not found");
}

// =================== Routes ===================
void routesForPortal() {
  server.on("/", HTTP_GET, handleSetupHtml);  // in portal, land on setup UI
  server.on("/index.html", HTTP_GET, handleIndex);
  server.on("/setup.html", HTTP_GET, handleSetupHtml);

  // static assets
  server.on("/app.js", HTTP_GET, []() {
    serveStaticFile("/app.js");
  });
  server.on("/setup.js", HTTP_GET, []() {
    serveStaticFile("/setup.js");
  });
  server.on("/styles.css", HTTP_GET, []() {
    serveStaticFile("/styles.css");
  });

  // json/api
  server.on("/iss.json", HTTP_GET, handleIssJson);
  server.on("/config.json", HTTP_GET, handleConfigJson);
  server.on("/scan.json", HTTP_GET, handleScanJson);
  server.on("/track.json", HTTP_GET, handleTrackJson);

  // actions
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/loc", HTTP_ANY, handleLoc);
  server.on("/savehome", HTTP_POST, handleSaveHome);

  server.begin();
}

void routesForNormal() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/index.html", HTTP_GET, handleIndex);
  server.on("/setup.html", HTTP_GET, handleSetupHtml);

  // static assets
  server.on("/app.js", HTTP_GET, []() {
    serveStaticFile("/app.js");
  });
  server.on("/setup.js", HTTP_GET, []() {
    serveStaticFile("/setup.js");
  });
  server.on("/styles.css", HTTP_GET, []() {
    serveStaticFile("/styles.css");
  });

  // json/api
  server.on("/iss.json", HTTP_GET, handleIssJson);
  server.on("/config.json", HTTP_GET, handleConfigJson);
  server.on("/scan.json", HTTP_GET, handleScanJson);
  server.on("/track.json", HTTP_GET, handleTrackJson);

  // actions
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/loc", HTTP_ANY, handleLoc);
  server.on("/savehome", HTTP_POST, handleSaveHome);

  server.begin();
  if (MDNS.begin(MDNS_NAME)) { MDNS.addService("http", "tcp", 80); }
}

// =================== Wi-Fi control ===================
void startPortal() {
  WiFi.disconnect(true, true);
  WiFi.scanDelete();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID().c_str(), nullptr);
  delay(100);
  IPAddress apIp = WiFi.softAPIP();
  dns.start(53, "*", apIp);
  drawPortalBanner(apIp);
  server.stop();
  routesForPortal();
  wifiState = WifiState::PORTAL;
}

bool tryConnectSTA() {
  String ssid, pass;
  bool haveSaved = loadWifiCreds(ssid, pass);
  if (!haveSaved && strlen(WIFI_SSID) > 0) {
    ssid = WIFI_SSID;
    pass = WIFI_PASSWORD;
  }
  if (!ssid.length()) {
    startPortal();
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.drawString("Wi-Fi...", 4, 150);

  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
    M5.Lcd.drawString(".", M5.Lcd.getCursorX() + 2, 150);
    delay(250);
  }
  M5.Lcd.fillRect(4, 150, 236, 20, BLACK);

  if (WiFi.status() == WL_CONNECTED) {
    server.stop();
    routesForNormal();
    M5.Lcd.setTextColor(GREEN, BLACK);
    M5.Lcd.drawString("Wi-Fi ok " + WiFi.localIP().toString(), 4, 150);
    beep(WIFI_BEEP_HZ, WIFI_BEEP_MS);
    wifiState = WifiState::STA_OK;
    lastStaOkMs = millis();
    return true;
  }
  startPortal();
  return false;
}

// =================== Setup / Loop ===================
void setup() {
  M5.begin();
  M5.Axp.ScreenBreath(15);
  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(BLACK);
  drawHeader();

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  // FS mount
  if (!LittleFS.begin(true)) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.drawString("LittleFS mount failed", 4, 130);
  }

  loadHomeFromNVS();
  tryConnectSTA();  // STA first; falls back to portal if needed
  lastFetch = 0;
}

void loop() {
  M5.update();

  // Web + DNS servicing
  server.handleClient();
  if (wifiState == WifiState::PORTAL) { dns.processNextRequest(); }

  // Buttons
  if (M5.BtnA.wasPressed()) {
    soundEnabled = !soundEnabled;
    drawHeader();
  }
  if (M5.BtnB.wasPressed()) { lastFetch = 0; }  // force ISS refresh

  // Auto-fallback to portal
  if (wifiState == WifiState::STA_OK) {
    if (WiFi.status() != WL_CONNECTED) {
      if ((millis() - lastStaOkMs) > WIFI_STALE_REVERT_MS) startPortal();
    } else {
      lastStaOkMs = millis();
    }
  }

  // ISS fetch only if STA OK
  const uint32_t now = millis();
  if (wifiState == WifiState::STA_OK && (now - lastFetch >= FETCH_INTERVAL_MS)) {
    lastFetch = now;

    double newLat, newLon;
    if (fetchISS(newLat, newLon)) {
      // Velocity + direction
      if (haveFix && prevSampleMs != 0) {
        uint32_t dtMs = now - prevSampleMs;
        if (dtMs >= 1000) {
          double dKm = greatCircleKm(issLat, issLon, newLat, newLon);
          double hours = dtMs / 3600000.0;
          double v = (hours > 0.0) ? (dKm / hours) : NAN;

          // ISS sanity (~27,600 km/h)
          if (!isnan(v) && v > 15000.0 && v < 40000.0) {
            velKmh = v;
            const double alpha = 0.25;
            if (velValid && !isnan(velKmhEma)) velKmhEma = alpha * velKmh + (1.0 - alpha) * velKmhEma;
            else {
              velKmhEma = velKmh;
              velValid = true;
            }
            double brg = initialBearingDeg(issLat, issLon, newLat, newLon);
            velDir8 = bearingTo8(brg);
          }
        }
      }

      prevLat = issLat;
      prevLon = issLon;
      prevSampleMs = now;
      issLat = newLat;
      issLon = newLon;
      haveFix = true;

      // Track capture (RAM + lazy flush to file)
      trackPush(now, issLat, issLon);
    }

    drawHeader();
  }

  // Draw UI (persist last known)
  if (haveFix) {
    double dist = greatCircleKm(homeLat, homeLon, issLat, issLon);

    drawValues(issLat, issLon, dist, velValid, velKmhEma, velDir8);
    drawMiniMap(issLat, issLon);
    drawProximityBar(dist);

    // Proximity logic (continuous beep + LED cadence) with hysteresis stop
    bool inClose = (dist <= RADIUS_KM);
    if (!wasClose && inClose) {
      wasClose = true;
      nextBeepAtMs = 0;  // force immediate
    } else if (wasClose && dist >= (RADIUS_KM + HYST_KM)) {
      wasClose = false;
      stopContinuousBeepNow();
      ledBlinking = false;
      ledOff();
    }

    if (wasClose) {
      uint32_t period = periodForDistance(dist);

      // Beep scheduling
      if (millis() >= nextBeepAtMs) {
        if (soundEnabled) startContinuousBeepNow();
        ledBlinking = true;
        ledOn();
        beepOffAtMs = millis() + ISS_BEEP_MS;
        nextBeepAtMs = millis() + period;
      }
      if (contBeepActive && millis() >= beepOffAtMs) stopContinuousBeepNow();

      // LED: turn off mid-period for a flash, even if sound is off
      uint32_t cycleStart = nextBeepAtMs - period;
      if (millis() - cycleStart >= (period / 2)) ledOff();
    }
  } else {
    // No ISS data yet
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(ORANGE, BLACK);
    if (wifiState == WifiState::PORTAL) {
      M5.Lcd.drawString("Setup Wi-Fi @:", 4, 60);
      String apInfo = String("http://") + WiFi.softAPIP().toString();
      M5.Lcd.drawString(apInfo, 4, 80);
      M5.Lcd.drawString("Open /setup.html", 4, 100);
    } else {
      M5.Lcd.drawString("Waiting for ISS...", 4, 60);
    }
    stopContinuousBeepNow();
    ledBlinking = false;
    ledOff();
  }

  // Periodic persistence flush
  trackFlushIfDue();

  delay(10);
}
