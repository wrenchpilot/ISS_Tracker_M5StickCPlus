/*
  ISS Tracker — M5StickC Plus (v2.0)
  -----------------------------------
  - Uses a single API:
      https://api.wheretheiss.at/v1/satellites/25544?units=kilometers  (HTTPS)
  - Serves a static UI from LittleFS:
      /index.html (map/telemetry), /setup.html, /app.js, /setup.js, /style.css
  - JSON endpoints:
      /iss.json         -> live telemetry + derived values
      /track.json?mins= -> persisted past path (reads track.ndjson)
      /predict.json     -> 1-hour prediction based on recent motion
      /scan.json        -> nearby Wi-Fi list
      /config.json      -> device + home info
      /loc (GET/POST)   -> set/persist new home
  - Track persistence:
      On every good ISS sample, append {"ts":<unix>,"lat":..,"lon":..}\n to /track.ndjson.
      Reads the last N minutes on demand; no RAM history required across reboots.
  - Draggable home (handled by UI -> /loc)
  - M5 on-device screen is still updated.
*/

#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // << REQUIRED for HTTPS
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESP.h>
#include <math.h>
#include "user_settings.h" // WIFI_SSID, WIFI_PASSWORD, HOME_LAT, HOME_LON, LOC_TOKEN (empty OK)

// ----------------- CONFIG -----------------
constexpr double RADIUS_KM = 800.0;
constexpr double HYST_KM = 200.0;
constexpr uint32_t FETCH_INTERVAL_MS = 5000;
constexpr uint16_t MIN_BEEP_PERIOD_MS = 220;
constexpr uint16_t MAX_BEEP_PERIOD_MS = 2800;

constexpr int BUZZER_PIN = 26;
constexpr uint8_t BUZZER_RES_BITS = 10;
constexpr uint32_t BUZZER_INIT_HZ = 4000;

constexpr int LED_PIN = 10; // active-low on M5StickC Plus
constexpr bool LED_ACTIVE_LOW = true;

constexpr uint16_t WIFI_BEEP_HZ = 1400;
constexpr uint16_t WIFI_BEEP_MS = 90;
constexpr uint16_t ISS_BEEP_HZ = 2300;
constexpr uint16_t ISS_BEEP_MS = 120;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_STALE_REVERT_MS = 45000;

static const char *MDNS_NAME = "iss";

// ---- API ----
static const char *ISS_URL = "https://api.wheretheiss.at/v1/satellites/25544?units=kilometers";

// ---- Files ----
static const char *TRACK_FILE = "/track.ndjson"; // each line: {"ts":<unix>,"lat":<deg>,"lon":<deg>}

// ----------------- STATE -----------------
WebServer server(80);
Preferences prefs;
DNSServer dns;

enum class WifiState
{
  STA_OK,
  PORTAL
};
WifiState wifiState = WifiState::PORTAL;

uint32_t lastFetch = 0;
uint32_t lastStaOkMs = 0;
bool soundEnabled = true;

double issLat = NAN, issLon = NAN; // latest
double issAltKm = NAN;             // altitude
double issVelKmh = NAN;            // instantaneous
double issVelEma = NAN;            // smoothed
bool issVelValid = false;
String issVis = "";     // visibility
double issFootKm = NAN; // footprint (km)
double solarLat = NAN, solarLon = NAN;
uint32_t issTs = 0;

// Prediction cache (6 points max for mini-map, every ~10 minutes = 1 hour)
struct PredPoint
{
  double lat;
  double lon;
};
PredPoint predCache[6];
int predCacheSize = 0;
uint32_t lastPredictCalc = 0;

double prevLat = NAN, prevLon = NAN;
uint32_t prevSampleMs = 0;

double homeLat = HOME_LAT;
double homeLon = HOME_LON;

bool haveFix = false;
bool wasClose = false;

bool ledState = false, ledBlinking = false, contBeepActive = false;
uint32_t nextBeepAtMs = 0, beepOffAtMs = 0;

// ----------------- M5CANVAS SPRITE (for screenshot capability) -----------------
// M5StickC Plus: 240x135 display. ST7735S controller doesn't support read operations.
// We use TFT_eSprite (sprite) - an in-memory framebuffer that captures ALL rendering (text + graphics).
// Draw to sprite → pushSprite to display → read sprite buffer for screenshots.
// Memory: 240 * 135 * 2 = 64,800 bytes (~63 KB RGB565)
TFT_eSprite canvas = TFT_eSprite(&M5.Lcd); // sprite backed by TFT_eSPI library

// ----------------- MATH HELPERS -----------------
static inline double toRad(double deg)
{
  return deg * M_PI / 180.0;
}
static inline double toDeg(double rad)
{
  return rad * 180.0 / M_PI;
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2)
{
  const double R = 6371.0;
  const double dLat = toRad(lat2 - lat1);
  const double dLon = toRad(lon2 - lon1);
  const double a = sin(dLat / 2) * sin(dLat / 2) + cos(toRad(lat1)) * cos(toRad(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

double initialBearingDeg(double lat1, double lon1, double lat2, double lon2)
{
  double phi1 = toRad(lat1), phi2 = toRad(lat2);
  double dLon = toRad(lon2 - lon1);
  double y = sin(dLon) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
  double th = atan2(y, x);
  double deg = toDeg(th);
  if (deg < 0)
    deg += 360.0;
  return deg;
}

String bearingTo8(double deg)
{
  static const char *d[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int i = (int)floor((deg + 22.5) / 45.0) % 8;
  return String(d[i]);
}

// Move a point along a great circle by distance(km) and bearing(deg)
void movePoint(double lat, double lon, double distKm, double bearingDeg, double &outLat, double &outLon)
{
  const double R = 6371.0;
  double δ = distKm / R;
  double θ = toRad(bearingDeg);
  double φ1 = toRad(lat), λ1 = toRad(lon);
  double sinφ1 = sin(φ1), cosφ1 = cos(φ1);
  double sinδ = sin(δ), cosδ = cos(δ);

  double sinφ2 = sinφ1 * cosδ + cosφ1 * sinδ * cos(θ);
  double φ2 = asin(sinφ2);
  double y = sin(θ) * sinδ * cosφ1;
  double x = cosδ - sinφ1 * sinφ2;
  double λ2 = λ1 + atan2(y, x);

  outLat = toDeg(φ2);
  outLon = fmod(toDeg(λ2) + 540.0, 360.0) - 180.0; // wrap
}

// Calculate simple 1-hour prediction based on current velocity & bearing
void calculatePrediction()
{
  if (isnan(prevLat) || isnan(prevLon) || isnan(issLat) || isnan(issLon))
  {
    predCacheSize = 0;
    return;
  }

  // Bearing from last two samples
  double brg = initialBearingDeg(prevLat, prevLon, issLat, issLon);

  // Estimate ground speed (km/h)
  double gspeed = NAN;
  if (prevSampleMs)
  {
    uint32_t dtMs = millis() - prevSampleMs;
    if (dtMs >= 1000)
    {
      double dKm = greatCircleKm(prevLat, prevLon, issLat, issLon);
      double hours = (double)dtMs / 3600000.0;
      if (hours > 0)
        gspeed = dKm / hours;
    }
  }
  if (isnan(gspeed))
  {
    // Fallback: use raw velocity with projection factor
    if (!isnan(issVelKmh))
      gspeed = issVelKmh * 0.85;
    else
      gspeed = 27500.0 * 0.85;
  }

  // Generate 6 prediction points at 10-minute intervals (1 hour total)
  predCacheSize = 6;
  for (int i = 0; i < predCacheSize; i++)
  {
    int minutes = (i + 1) * 10; // 10, 20, 30, 40, 50, 60 minutes
    double distKm = gspeed * (minutes / 60.0);
    movePoint(issLat, issLon, distKm, brg, predCache[i].lat, predCache[i].lon);
  }

  lastPredictCalc = millis();
}

// ----------------- LED/BUZZER -----------------
inline void ledWrite(bool on)
{
  digitalWrite(LED_PIN, (LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW)));
}
inline void ledOff()
{
  ledState = false;
  ledWrite(false);
}
inline void ledOn()
{
  ledState = true;
  ledWrite(true);
}

static inline void buzzerAttachIfNeeded()
{
  ledcAttach(BUZZER_PIN, BUZZER_INIT_HZ, BUZZER_RES_BITS);
}
static inline void buzzerDetachIfNeeded()
{
  ledcDetach(BUZZER_PIN);
}
static inline void toneStart(uint16_t f)
{
  ledcWriteTone(BUZZER_PIN, f);
}
static inline void toneStop()
{
  ledcWriteTone(BUZZER_PIN, 0);
}

void beep(uint16_t hz, uint16_t ms)
{
  if (!soundEnabled)
    return;
  buzzerAttachIfNeeded();
  toneStart(hz);
  delay(ms);
  toneStop();
  buzzerDetachIfNeeded();
}

uint32_t periodForDistance(double d)
{
  if (d < 0)
    d = 0;
  if (d > RADIUS_KM)
    d = RADIUS_KM;
  double t = d / RADIUS_KM;
  double p = MIN_BEEP_PERIOD_MS + t * (MAX_BEEP_PERIOD_MS - MIN_BEEP_PERIOD_MS);
  if (p < MIN_BEEP_PERIOD_MS)
    p = MIN_BEEP_PERIOD_MS;
  if (p > MAX_BEEP_PERIOD_MS)
    p = MAX_BEEP_PERIOD_MS;
  return (uint32_t)p;
}

void startContinuousBeepNow()
{
  if (!soundEnabled)
    return;
  buzzerAttachIfNeeded();
  toneStart(ISS_BEEP_HZ);
  contBeepActive = true;
}
void stopContinuousBeepNow()
{
  if (!contBeepActive)
    return;
  toneStop();
  buzzerDetachIfNeeded();
  contBeepActive = false;
}

// ----------------- M5 UI -----------------
void drawHeader()
{
  canvas.fillSprite(BLACK);
  canvas.setTextColor(WHITE, BLACK);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextSize(2);
  canvas.drawString("ISS Tracker", 4, 2);
  canvas.setTextSize(1);
  canvas.drawString(soundEnabled ? "A:Sound ON" : "A:Sound OFF", 175, 4);
  canvas.drawString("B:Refresh", 175, 16);
}

void drawValues(double lat, double lon, double distKm, bool showVel, double vKmh, const String &dir8)
{
  int y = 28;
  canvas.setTextSize(2);
  canvas.setTextColor(YELLOW, BLACK);
  canvas.drawString("Lat:", 4, y);
  canvas.drawFloat(lat, 4, 56, y);
  y += 22;
  canvas.drawString("Lon:", 4, y);
  canvas.drawFloat(lon, 4, 56, y);
  y += 22;

  // Velocity line + Altitude afterwards
  canvas.setTextColor(WHITE, BLACK);
  canvas.drawString("Vel:", 4, y);
  if (showVel)
  {
    canvas.drawFloat(vKmh, 1, 56, y);
    canvas.drawString("km/h", 146, y);
    if (dir8.length())
      canvas.drawString(dir8.c_str(), 201, y);
  }
  else
  {
    canvas.drawString("---", 56, y);
  }
  y += 22;

  canvas.setTextColor(CYAN, BLACK);
  canvas.drawString("Dis:", 4, y);
  canvas.drawFloat(distKm, 1, 56, y);
  canvas.drawString("km", 146, y);
}

void drawProximityBar(double distKm)
{
  const int barX = 4, barY = 116, barW = canvas.width() - 8, barH = 18;
  String status;
  uint16_t colFill;
  if (distKm <= RADIUS_KM)
  {
    status = "Close";
    colFill = GREEN;
  }
  else if (distKm <= 1200)
  {
    status = "Near";
    colFill = YELLOW;
  }
  else if (distKm <= 1600)
  {
    status = "Mid";
    colFill = ORANGE;
  }
  else
  {
    status = "Far";
    colFill = RED;
  }

  canvas.drawRect(barX, barY, barW, barH, WHITE);
  double maxKm = 2000.0;
  int fillw = (distKm > 1600.0) ? (barW - 2) : (int)((maxKm - max(0.0, distKm)) / maxKm * (barW - 2));
  canvas.fillRect(barX + 1, barY + 1, fillw, barH - 2, colFill);

  for (int km = 500; km < 2000; km += 500)
  {
    int tx = barX + 1 + (int)((maxKm - km) / maxKm * (barW - 2));
    canvas.drawLine(tx, barY, tx, barY + barH, DARKGREY);
  }
  canvas.setTextSize(2);
  canvas.setTextColor(WHITE, BLACK);
  int statusY = barY + (barH - 8) / 2;
  canvas.drawCentreString(status, canvas.width() / 2, statusY - 4, 1);
}

void drawMiniMap(double curLat, double curLon)
{
  const bool SHOW_MINIMAP = true;
  if (!SHOW_MINIMAP)
    return;
  const int mapX = 158, mapY = 26, mapW = 78, mapH = 42;
  canvas.fillRect(mapX, mapY, mapW, mapH, TFT_NAVY);
  canvas.drawRect(mapX, mapY, mapW, mapH, DARKGREY);

  for (int lon = -150; lon <= 180; lon += 30)
  {
    int x = mapX + (int)((lon + 180.0) / 360.0 * (mapW - 1));
    canvas.drawLine(x, mapY, x, mapY + mapH - 1, DARKGREY);
  }
  for (int lat = -60; lat <= 60; lat += 15)
  {
    int y = mapY + (int)((90.0 - (double)lat) / 180.0 * (mapH - 1));
    canvas.drawLine(mapX, y, mapX + mapW - 1, y, DARKGREY);
  }
  auto pxX = [&](double lon) -> int
  {
    double L = fmod((lon + 540.0), 360.0) - 180.0;
    return mapX + (int)((L + 180.0) / 360.0 * (mapW - 1));
  };
  auto pxY = [&](double lat) -> int
  {
    double c = lat;
    if (c > 90)
      c = 90;
    if (c < -90)
      c = -90;
    return mapY + (int)((90.0 - c) / 180.0 * (mapH - 1));
  };

  int hx = pxX(homeLon), hy = pxY(homeLat);
  canvas.fillCircle(hx, hy, 2, GREEN);
  canvas.drawPixel(hx, hy, WHITE);
  int ix = pxX(curLon), iy = pxY(curLat);
  canvas.fillCircle(ix, iy, 2, RED);
  canvas.drawPixel(ix, iy, WHITE);

  // Draw sun position (yellow dot)
  if (!isnan(solarLat) && !isnan(solarLon)) {
    int sx = pxX(solarLon), sy = pxY(solarLat);
    canvas.fillCircle(sx, sy, 2, YELLOW);
    canvas.drawPixel(sx, sy, WHITE);
  }

  // Draw prediction line (red dotted)
  if (predCacheSize > 0)
  {
    for (int i = 0; i < predCacheSize - 1; i++)
    {
      double lon1 = predCache[i].lon, lon2 = predCache[i + 1].lon;
      // Skip segment if it crosses dateline (longitude jump > 180°)
      if (abs(lon2 - lon1) > 180.0)
        continue;

      int x1 = pxX(lon1), y1 = pxY(predCache[i].lat);
      int x2 = pxX(lon2), y2 = pxY(predCache[i + 1].lat);

      // Simple dotted line: draw every other pixel
      int dx = abs(x2 - x1), dy = abs(y2 - y1);
      int sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1;
      int err = dx - dy;
      int x = x1, y = y1;
      int step = 0;
      while (true)
      {
        if (step % 3 != 0)
          canvas.drawPixel(x, y, RED); // dotted effect
        if (x == x2 && y == y2)
          break;
        int e2 = 2 * err;
        if (e2 > -dy)
        {
          err -= dy;
          x += sx;
        }
        if (e2 < dx)
        {
          err += dx;
          y += sy;
        }
        step++;
      }
    }
  }

  // canvas.setTextColor(WHITE, TFT_NAVY);
  // canvas.setTextSize(1);
  // canvas.drawString("MAP", mapX + 2, mapY + 2);
}

// ----------------- PERSISTENCE -----------------
void loadHomeFromNVS()
{
  prefs.begin("iss", true);
  double la = prefs.getDouble("homeLat", NAN), lo = prefs.getDouble("homeLon", NAN);
  prefs.end();
  if (!isnan(la) && !isnan(lo))
  {
    homeLat = la;
    homeLon = lo;
  }
}
void saveHomeToNVS(double la, double lo)
{
  prefs.begin("iss", false);
  prefs.putDouble("homeLat", la);
  prefs.putDouble("homeLon", lo);
  prefs.end();
}

// Wi-Fi creds
bool loadWifiCreds(String &ssid, String &pass)
{
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", ""), p = prefs.getString("pass", "");
  prefs.end();
  if (s.length())
  {
    ssid = s;
    pass = p;
    return true;
  }
  return false;
}
void saveWifiCreds(const String &s, const String &p)
{
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}
void forgetWifiCreds()
{
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ----------------- AUTH -----------------
bool tokenOk(const String &bearerOrQuery)
{
  if (LOC_TOKEN[0] == 0)
    return true;
  return bearerOrQuery == LOC_TOKEN;
}

// ----------------- TRACK PERSISTENCE (NDJSON) -----------------
void appendTrackPoint(uint32_t ts, double lat, double lon)
{
  File f = LittleFS.open(TRACK_FILE, "a");
  if (!f)
    return;
  DynamicJsonDocument dj(96);
  dj["ts"] = ts;
  dj["lat"] = lat;
  dj["lon"] = lon;
  String line;
  serializeJson(dj, line);
  line += "\n";
  f.print(line);
  f.close();
}

// ----------------- ISS NETWORKING -----------------
bool fetchISS(double &outLat, double &outLon)
{
  WiFiClientSecure client;
  client.setInsecure(); // HTTPS without certificate validation

  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(client, ISS_URL))
    return false;

  bool ok = false;
  int code = http.GET();
  if (code == HTTP_CODE_OK)
  {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err)
    {
      outLat = doc["latitude"] | NAN;
      outLon = doc["longitude"] | NAN;

      issAltKm = doc["altitude"] | NAN;
      issVelKmh = doc["velocity"] | NAN;
      issVis = (const char *)(doc["visibility"] | "");
      issFootKm = doc["footprint"] | NAN;
      issTs = doc["timestamp"] | (uint32_t)0;
      solarLat = doc["solar_lat"] | NAN;
      solarLon = doc["solar_lon"] | NAN;

      ok = !(isnan(outLat) || isnan(outLon));
    }
  }
  http.end();
  return ok;
}

// ----------------- WIFI/AP -----------------
String apSSID()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "ISS-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}
void drawPortalBanner(IPAddress apIp)
{
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(ORANGE, BLACK);
  canvas.fillRect(4, 150, 236, 20, BLACK);
  String s = "Setup AP: " + apSSID() + "  " + apIp.toString();
  M5.Lcd.drawString(s, 4, 150);
}

// ----------------- STATIC FILE SERVER -----------------
String contentTypeFor(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html; charset=utf-8";
  if (path.endsWith(".css"))
    return "text/css; charset=utf-8";
  if (path.endsWith(".js"))
    return "application/javascript; charset=utf-8";
  if (path.endsWith(".json"))
    return "application/json; charset=utf-8";
  if (path.endsWith(".png"))
    return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg"))
    return "image/jpeg";
  if (path.endsWith(".svg"))
    return "image/svg+xml";
  if (path.endsWith(".bmp"))
    return "image/bmp";
  return "text/plain; charset=utf-8";
}

bool serveStaticFile(String path)
{
  String gz = path + ".gz";
  if (LittleFS.exists(gz))
  {
    File f = LittleFS.open(gz, "r");
    if (!f)
      return false;
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.streamFile(f, contentTypeFor(path));
    f.close();
    return true;
  }
  if (!LittleFS.exists(path))
    return false;
  File f = LittleFS.open(path, "r");
  if (!f)
    return false;
  server.sendHeader("Cache-Control", "Cache-Control: public, max-age=31536000, immutable");
  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

// Write current screen as a 24-bit BMP to LittleFS. Returns true on success.
bool writeScreenBmpToFile(const char *path)
{
  uint16_t w = canvas.width();
  uint16_t h = canvas.height();
  uint32_t rowBytes = (uint32_t)w * 3;
  uint32_t pad = (4 - (rowBytes & 3)) & 3;
  uint32_t bmpDataSize = (rowBytes + pad) * h;
  uint32_t fileSize = 14 + 40 + bmpDataSize;

  File f = LittleFS.open(path, "w");
  if (!f)
    return false;

  // BITMAPFILEHEADER
  uint8_t fileHeader[14] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0};
  fileHeader[2] = (uint8_t)(fileSize & 0xFF);
  fileHeader[3] = (uint8_t)((fileSize >> 8) & 0xFF);
  fileHeader[4] = (uint8_t)((fileSize >> 16) & 0xFF);
  fileHeader[5] = (uint8_t)((fileSize >> 24) & 0xFF);
  if (f.write(fileHeader, 14) != 14)
  {
    f.close();
    return false;
  }

  // BITMAPINFOHEADER
  uint8_t info[40];
  memset(info, 0, 40);
  info[0] = 40;
  uint32_t ww = w;
  info[4] = (uint8_t)(ww & 0xFF);
  info[5] = (uint8_t)((ww >> 8) & 0xFF);
  info[6] = (uint8_t)((ww >> 16) & 0xFF);
  info[7] = (uint8_t)((ww >> 24) & 0xFF);
  uint32_t hh = h;
  info[8] = (uint8_t)(hh & 0xFF);
  info[9] = (uint8_t)((hh >> 8) & 0xFF);
  info[10] = (uint8_t)((hh >> 16) & 0xFF);
  info[11] = (uint8_t)((hh >> 24) & 0xFF);
  info[12] = 1;
  info[14] = 24;
  info[20] = (uint8_t)(bmpDataSize & 0xFF);
  info[21] = (uint8_t)((bmpDataSize >> 8) & 0xFF);
  info[22] = (uint8_t)((bmpDataSize >> 16) & 0xFF);
  info[23] = (uint8_t)((bmpDataSize >> 24) & 0xFF);
  if (f.write(info, 40) != 40)
  {
    f.close();
    return false;
  }

  uint8_t padbuf[3] = {0, 0, 0};

  // Read from M5Canvas sprite using readPixelValue() (RGB565 format)
  // BMP rows are bottom-to-top, pixels are BGR
  for (int y = (int)h - 1; y >= 0; --y)
  {
    for (int x = 0; x < (int)w; ++x)
    {
      // Get RGB565 from sprite buffer
      uint16_t rgb565 = (uint16_t)canvas.readPixel(x, y);

      // Convert RGB565 to RGB888
      uint8_t r = ((rgb565 >> 11) & 0x1F) << 3; // 5 bits -> 8 bits
      uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;  // 6 bits -> 8 bits
      uint8_t b = (rgb565 & 0x1F) << 3;         // 5 bits -> 8 bits

      // Expand to full 8-bit range
      r |= (r >> 5);
      g |= (g >> 6);
      b |= (b >> 5);

      // Write as B,G,R for BMP format
      uint8_t px[3] = {b, g, r};
      if (f.write(px, 3) != 3)
      {
        f.close();
        return false;
      }
    }
    // write padding if needed
    if (pad)
    {
      if (f.write(padbuf, pad) != (int)pad)
      {
        f.close();
        return false;
      }
    }
  }
  f.close();
  return true;
}

// ----------------- JSON ENDPOINTS -----------------
void handleIssJson()
{
  DynamicJsonDocument doc(512);
  doc["haveFix"] = haveFix;

  if (haveFix)
  {
    JsonObject iss = doc.createNestedObject("iss");
    iss["lat"] = issLat;
    iss["lon"] = issLon;
    iss["alt"] = issAltKm;
    // publish both raw and EMA velocity
    if (issVelValid)
      iss["vel"] = issVelEma;
    iss["vel_raw"] = issVelKmh;

    // Direction only if we have prev sample
    if (!isnan(prevLat) && !isnan(prevLon))
    {
      double brg = initialBearingDeg(prevLat, prevLon, issLat, issLon);
      iss["dir"] = bearingTo8(brg);
    }

    iss["age_ms"] = (uint32_t)(millis() - prevSampleMs);
    iss["vis"] = issVis;
    iss["foot_km"] = issFootKm;
    iss["ts"] = issTs;
    iss["solar_lat"] = solarLat;
    iss["solar_lon"] = solarLon;

    double dist = greatCircleKm(homeLat, homeLon, issLat, issLon);
    iss["dist_km"] = dist;
  }

  JsonObject home = doc.createNestedObject("home");
  home["lat"] = homeLat;
  home["lon"] = homeLon;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// Return last N minutes of track from NDJSON
void handleTrackJson()
{
  int mins = 60;
  if (server.hasArg("mins"))
  {
    int m = server.arg("mins").toInt();
    if (m > 0 && m <= 1440)
      mins = m;
  }
  uint32_t cutoff = (millis() / 1000) + (issTs ? 0 : 0); // prefer file timestamps; no offset needed
  if (issTs)
    cutoff = issTs; // current unix ts
  if (cutoff > (uint32_t)mins * 60)
    cutoff -= (uint32_t)mins * 60;
  else
    cutoff = 0;

  String out = "[";
  bool first = true;

  File f = LittleFS.open(TRACK_FILE, "r");
  if (f)
  {
    while (f.available())
    {
      String line = f.readStringUntil('\n');
      if (line.length() < 8)
        continue;
      DynamicJsonDocument dj(96);
      if (deserializeJson(dj, line) == DeserializationError::Ok)
      {
        uint32_t ts = dj["ts"] | (uint32_t)0;
        double la = dj["lat"] | NAN;
        double lo = dj["lon"] | NAN;
        if (ts >= cutoff && !isnan(la) && !isnan(lo))
        {
          if (!first)
            out += ",";
          first = false;
          DynamicJsonDocument row(64);
          row["ts"] = ts;
          row["lat"] = la;
          row["lon"] = lo;
          String s;
          serializeJson(row, s);
          out += s;
        }
      }
    }
    f.close();
  }
  out += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// Very light 1-hour ground-track prediction based on recent motion vector
void handlePredictJson()
{
  // Need two samples to estimate motion
  if (isnan(prevLat) || isnan(prevLon) || isnan(issLat) || isnan(issLon))
  {
    server.send(200, "application/json", "[]");
    return;
  }
  // Bearing from last two points
  double brg = initialBearingDeg(prevLat, prevLon, issLat, issLon);

  // Estimate ground speed (km/h) from last interval if available; else fall back to raw velocity * cos(lat) as crude proj
  double gspeed = NAN;
  if (prevSampleMs)
  {
    uint32_t dtMs = millis() - prevSampleMs;
    if (dtMs >= 1000)
    {
      double dKm = greatCircleKm(prevLat, prevLon, issLat, issLon);
      double hours = (double)dtMs / 3600000.0;
      if (hours > 0)
        gspeed = dKm / hours;
    }
  }
  if (isnan(gspeed))
  {
    // crude: project orbital speed to ground speed; ignore winds, etc.
    if (!isnan(issVelKmh))
      gspeed = issVelKmh * 0.85; // heuristic
    else
      gspeed = 27500.0 * 0.85;
  }

  // Produce points every 60s for 60 minutes along great circle
  String out = "[";
  bool first = true;
  double lat = issLat, lon = issLon;
  for (int s = 60; s <= 3600; s += 60)
  {
    double distKm = gspeed * (s / 3600.0);
    double yl, yo;
    movePoint(issLat, issLon, distKm, brg, yl, yo);
    if (!first)
      out += ",";
    first = false;
    DynamicJsonDocument row(64);
    row["lat"] = yl;
    row["lon"] = yo;
    String js;
    serializeJson(row, js);
    out += js;
  }
  out += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleConfigJson()
{
  DynamicJsonDocument doc(192);
  doc["wifi"]["ssid"] = WiFi.SSID();
  doc["wifi"]["ip"] = WiFi.localIP().toString();
  doc["home"]["lat"] = homeLat;
  doc["home"]["lon"] = homeLon;
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleScanJson() {
  // Always do a fresh synchronous scan - ONLY reliable method in AP_STA mode
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

// ----- legacy /loc and /savehome -----
void handleLoc()
{
  String tok = "";
  if (server.hasHeader("Authorization"))
  {
    String h = server.header("Authorization");
    if (h.startsWith("Bearer "))
      tok = h.substring(7);
  }
  if (tok == "" && server.hasArg("token"))
    tok = server.arg("token");
  if (!tokenOk(tok))
  {
    server.send(401, "application/json", "{\"err\":\"unauthorized\"}");
    return;
  }

  double lat = NAN, lon = NAN;
  if (server.method() == HTTP_GET)
  {
    if (server.hasArg("lat"))
      lat = server.arg("lat").toDouble();
    if (server.hasArg("lon"))
      lon = server.arg("lon").toDouble();
  }
  else
  {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok)
    {
      lat = doc["lat"] | NAN;
      lon = doc["lon"] | NAN;
      if (tok == "" && doc.containsKey("token"))
        tok = (const char *)doc["token"];
      if (!tokenOk(tok))
      {
        server.send(401, "application/json", "{\"err\":\"unauthorized\"}");
        return;
      }
    }
  }
  if (isnan(lat) || isnan(lon))
  {
    server.send(400, "application/json", "{\"err\":\"lat/lon required\"}");
    return;
  }

  homeLat = lat;
  homeLon = lon;
  saveHomeToNVS(lat, lon);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveHome()
{
  double lat = NAN, lon = NAN;
  if (server.hasArg("lat"))
    lat = server.arg("lat").toDouble();
  if (server.hasArg("lon"))
    lon = server.arg("lon").toDouble();
  if (isnan(lat) || isnan(lon))
  {
    server.send(400, "text/plain; charset=utf-8", "lat/lon required");
    return;
  }
  homeLat = lat;
  homeLon = lon;
  saveHomeToNVS(lat, lon);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", "<meta http-equiv='refresh' content='1;url=/'/>Saved.");
}

// ----------------- STATIC ROUTES -----------------
void handleIndex()
{
  if (!serveStaticFile("/index.html"))
    server.send(404, "text/plain", "index.html not found");
}
void handleSetupHtml()
{
  if (!serveStaticFile("/setup.html"))
    server.send(404, "text/plain", "setup.html not found");
}
void handleHomeRedirect()
{
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ----------------- ROUTE SETS -----------------
void routesForPortal()
{
  server.on("/", HTTP_GET, handleSetupHtml); // portal lands on setup
  server.on("/setup.html", HTTP_GET, handleSetupHtml);
  server.on("/index.html", HTTP_GET, handleIndex);
  server.on("/home", HTTP_GET, handleHomeRedirect);

  server.on("/favicon.ico", HTTP_GET, []()
            { serveStaticFile("/favicon.ico"); });
  server.on("/app.js", HTTP_GET, []()
            { serveStaticFile("/app.js"); });
  server.on("/setup.js", HTTP_GET, []()
            { serveStaticFile("/setup.js"); });
  server.on("/bootstrap.bundle.min.js", HTTP_GET, []()
            { serveStaticFile("/bootstrap.bundle.min.js"); });
  server.on("/leaflet.js", HTTP_GET, []()
            { serveStaticFile("/leaflet.js"); });
  server.on("/leaflet.terminator", HTTP_GET, []()
            { serveStaticFile("/leaflet.terminator"); });
  server.on("/style.css", HTTP_GET, []()
            { serveStaticFile("/style.css"); });
    server.on("/bootstrap.min.css", HTTP_GET, []()
            { serveStaticFile("/bootstrap.min.css"); });
    server.on("/leaflet.css", HTTP_GET, []()
            { serveStaticFile("/leaflet.css"); });

  server.on("/iss.json", HTTP_GET, handleIssJson);
  server.on("/track.json", HTTP_GET, handleTrackJson);
  server.on("/predict.json", HTTP_GET, handlePredictJson);
  server.on("/config.json", HTTP_GET, handleConfigJson);
  server.on("/scan.json", HTTP_GET, handleScanJson);

  // serve a BMP capture of the current screen (write to LittleFS then serve)
  server.on("/screen.bmp", HTTP_GET, []()
            {
    const char* path = "/screen.bmp";
    if (!writeScreenBmpToFile(path)) {
      server.send(500, "text/plain", "failed to create bmp");
      return;
    }
    if (!serveStaticFile(String(path))) {
      server.send(500, "text/plain", "failed to serve bmp");
    } });

  // (screen.bmp route already registered above for portal; normal routes added below)

  server.on("/save", HTTP_POST, []()
            {
    String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
    String pass = server.hasArg("pass") ? server.arg("pass") : "";
    if (!ssid.length()) {
      server.send(400, "text/plain", "SSID required");
      return;
    }
    saveWifiCreds(ssid, pass);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", "<meta http-equiv='refresh' content='3;url=/'/>Saved. Rebooting…");
    delay(750);
    ESP.restart(); });
  server.on("/forget", HTTP_POST, []()
            {
    forgetWifiCreds();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", "<meta http-equiv='refresh' content='3;url=/setup.html'/>Forgotten. Rebooting…");
    delay(750);
    ESP.restart(); });

  server.on("/loc", HTTP_ANY, handleLoc);
  server.on("/savehome", HTTP_POST, handleSaveHome);

  server.begin();
}

void routesForNormal()
{
  server.on("/", HTTP_GET, handleIndex);
  server.on("/index.html", HTTP_GET, handleIndex);
  server.on("/setup", HTTP_GET, handleSetupHtml);
  server.on("/setup.html", HTTP_GET, handleSetupHtml);
  server.on("/home", HTTP_GET, handleHomeRedirect);

  server.on("/favicon.ico", HTTP_GET, []()
            { serveStaticFile("/favicon.ico"); });
  server.on("/app.js", HTTP_GET, []()
            { serveStaticFile("/app.js"); });
  server.on("/setup.js", HTTP_GET, []()
            { serveStaticFile("/setup.js"); });
  server.on("/bootstrap.bundle.min.js", HTTP_GET, []()
            { serveStaticFile("/bootstrap.bundle.min.js"); });
  server.on("/leaflet.js", HTTP_GET, []()
            { serveStaticFile("/leaflet.js"); });
  server.on("/leaflet.terminator", HTTP_GET, []()
            { serveStaticFile("/leaflet.terminator"); });
  server.on("/style.css", HTTP_GET, []()
            { serveStaticFile("/style.css"); });
    server.on("/bootstrap.min.css", HTTP_GET, []()
            { serveStaticFile("/bootstrap.min.css"); });
    server.on("/leaflet.css", HTTP_GET, []()
            { serveStaticFile("/leaflet.css"); });

  server.on("/iss.json", HTTP_GET, handleIssJson);
  server.on("/track.json", HTTP_GET, handleTrackJson);
  server.on("/predict.json", HTTP_GET, handlePredictJson);
  server.on("/config.json", HTTP_GET, handleConfigJson);
  server.on("/scan.json", HTTP_GET, handleScanJson);

  // ensure /screen.bmp is available in normal routes (STA mode)
  server.on("/screen.bmp", HTTP_GET, []()
            {
    const char* path = "/screen.bmp";
    if (!writeScreenBmpToFile(path)) {
      server.send(500, "text/plain", "failed to create bmp");
      return;
    }
    if (!serveStaticFile(String(path))) {
      server.send(500, "text/plain", "failed to serve bmp");
    } });

  server.on("/loc", HTTP_ANY, handleLoc);
  server.on("/savehome", HTTP_POST, handleSaveHome);

  server.begin();
  if (MDNS.begin(MDNS_NAME))
  {
    MDNS.addService("http", "tcp", 80);
  }
}

// =================== Wi-Fi control ===================
void startPortal() {
  // WiFi.disconnect(false) was already called in tryConnectSTA() after failed connection
  // Now safe to switch to AP_STA mode and start AP
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID().c_str(), nullptr);
  delay(500);  // AP initialization delay
  
  IPAddress apIp = WiFi.softAPIP();
  dns.start(53, "*", apIp);
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

  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    server.stop();
    routesForNormal();
    beep(WIFI_BEEP_HZ, WIFI_BEEP_MS);
    wifiState = WifiState::STA_OK;
    lastStaOkMs = millis();
    return true;
  }
  
  // CRITICAL FIX for ESP32 issue #8916:
  // Connection failed - radio is stuck in "connecting" state
  // MUST call disconnect(false) HERE (after failed connect, BEFORE mode switch)
  // This clears the stuck state without disabling WiFi radio
  WiFi.disconnect(false);
  delay(100);  // Give WiFi stack time to process disconnect
  
  startPortal();
  return false;
}

// ----------------- SETUP/LOOP -----------------
void setup()
{
  M5.begin();
  M5.Axp.ScreenBreath(15);
  M5.Lcd.setRotation(3);

  // Create M5Canvas sprite (240x135 RGB565 in-memory framebuffer)
  canvas.createSprite(240, 135);
  canvas.fillSprite(BLACK);

  drawHeader();
  canvas.pushSprite(0, 0); // Push sprite to display

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  if (!LittleFS.begin(true))
  {
    canvas.setTextColor(RED, BLACK);
    canvas.drawString("LittleFS mount failed", 4, 130);
    canvas.pushSprite(0, 0);
  }

  // CRITICAL: Clean WiFi initialization to prevent 5-minute AP startup delays
  // This clears any stuck/corrupted WiFi state from previous sessions
  WiFi.mode(WIFI_OFF);
  delay(100);
  
  loadHomeFromNVS();
  tryConnectSTA();
  lastFetch = 0;
}

void loop()
{
  M5.update();
  server.handleClient();
  if (wifiState == WifiState::PORTAL)
  {
    dns.processNextRequest();
  }

  if (M5.BtnA.wasPressed())
  {
    soundEnabled = !soundEnabled;
    drawHeader();
    canvas.pushSprite(0, 0);
  }
  if (M5.BtnB.wasPressed())
  {
    lastFetch = 0;
  }

  if (wifiState == WifiState::STA_OK)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      if ((millis() - lastStaOkMs) > WIFI_STALE_REVERT_MS)
        startPortal();
    }
    else
    {
      lastStaOkMs = millis();
    }
  }

  const uint32_t now = millis();
  if (wifiState == WifiState::STA_OK && (now - lastFetch >= FETCH_INTERVAL_MS))
  {
    lastFetch = now;
    double newLat, newLon;
    if (fetchISS(newLat, newLon))
    {
      // velocity smoothing + direction
      if (haveFix && prevSampleMs != 0)
      {
        uint32_t dtMs = now - prevSampleMs;
        if (dtMs >= 1000)
        {
          double dKm = greatCircleKm(issLat, issLon, newLat, newLon);
          double hours = dtMs / 3600000.0;
          double v = (hours > 0.0) ? (dKm / hours) : NAN;
          if (!isnan(v) && v > 15000.0 && v < 40000.0)
          {
            const double alpha = 0.25;
            if (issVelValid && !isnan(issVelEma))
              issVelEma = alpha * issVelKmh + (1.0 - alpha) * issVelEma;
            else
            {
              issVelEma = issVelKmh;
              issVelValid = true;
            }
          }
        }
      }

      prevLat = issLat;
      prevLon = issLon;
      prevSampleMs = now;
      issLat = newLat;
      issLon = newLon;
      haveFix = true;

      // Calculate prediction (update every fetch or if stale > 60s)
      if (lastPredictCalc == 0 || (now - lastPredictCalc) > 60000)
      {
        calculatePrediction();
      }

      // persist current sample to track file
      if (issTs != 0 && !isnan(issLat) && !isnan(issLon))
        appendTrackPoint(issTs, issLat, issLon);
    }
    drawHeader();
  }

  if (haveFix)
  {
    double dist = greatCircleKm(homeLat, homeLon, issLat, issLon);
    String dir8 = (!isnan(prevLat) && !isnan(prevLon)) ? bearingTo8(initialBearingDeg(prevLat, prevLon, issLat, issLon)) : "";
    drawValues(issLat, issLon, dist, issVelValid, issVelEma, dir8);
    drawMiniMap(issLat, issLon);
    drawProximityBar(dist);
    canvas.pushSprite(0, 0); // Push complete frame to display

    bool inClose = (dist <= RADIUS_KM);
    if (!wasClose && inClose)
    {
      wasClose = true;
      nextBeepAtMs = 0;
    }
    else if (wasClose && dist >= (RADIUS_KM + HYST_KM))
    {
      wasClose = false;
      stopContinuousBeepNow();
      ledBlinking = false;
      ledOff();
    }
    if (wasClose)
    {
      uint32_t period = periodForDistance(dist);
      if (millis() >= nextBeepAtMs)
      {
        if (soundEnabled)
          startContinuousBeepNow();
        ledBlinking = true;
        ledOn();
        beepOffAtMs = millis() + ISS_BEEP_MS;
        nextBeepAtMs = millis() + period;
      }
      if (contBeepActive && millis() >= beepOffAtMs)
        stopContinuousBeepNow();
      uint32_t cycleStart = nextBeepAtMs - period;
      if (millis() - cycleStart >= (period / 2))
        ledOff();
    }
  }
  else
  {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(ORANGE, BLACK);
    if (wifiState == WifiState::PORTAL)
    {
      M5.Lcd.drawString("Setup Wi-Fi @:", 4, 60);
      String apInfo = String("http://") + WiFi.softAPIP().toString();
      String apName = String("SSID:") + WiFi.softAPSSID();
      M5.Lcd.drawString(apName, 4, 80);
      M5.Lcd.drawString(apInfo, 4, 100);
    }
    else
    {
      M5.Lcd.drawString("Waiting for ISS...", 4, 60);
    }
    stopContinuousBeepNow();
    ledBlinking = false;
    ledOff();
  }

  delay(10);
}
