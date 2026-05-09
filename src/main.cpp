#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#if __has_include("Config.h")
#include "Config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef EVCC_HOST
#define EVCC_HOST "evcc.local"
#endif
#ifndef EVCC_PORT
#define EVCC_PORT 7070
#endif
#ifndef EVCC_LOADPOINT_ID
#define EVCC_LOADPOINT_ID 1
#endif
#ifndef WALLBOX_LABEL
#define WALLBOX_LABEL "Garage"
#endif
#ifndef APP_VERSION
#define APP_VERSION "0.1.0-dev"
#endif
#ifndef DISPLAY_ROTATION
#define DISPLAY_ROTATION 0
#endif
#ifndef TOUCH_ROTATION
#define TOUCH_ROTATION 0
#endif
#ifndef GFXFF
#define GFXFF 1
#endif

namespace {

constexpr uint16_t ScreenW = 240;
constexpr uint16_t ScreenH = 320;

constexpr uint8_t TouchIrq = 36;
constexpr uint8_t TouchMosi = 32;
constexpr uint8_t TouchMiso = 39;
constexpr uint8_t TouchClk = 25;
constexpr uint8_t TouchCs = 33;

constexpr int TouchMinX = 200;
constexpr int TouchMaxX = 3700;
constexpr int TouchMinY = 240;
constexpr int TouchMaxY = 3800;

constexpr uint32_t PollIntervalMs = 5000;
constexpr uint32_t WifiRetryMs = 10000;
constexpr uint32_t TouchDebounceMs = 450;
constexpr uint32_t EvccTimeoutMs = 8000;

constexpr uint16_t ColorBg = 0xF79E;
constexpr uint16_t ColorCard = TFT_WHITE;
constexpr uint16_t ColorLine = 0xD6DA;
constexpr uint16_t ColorSoft = 0xEF5D;
constexpr uint16_t ColorText = 0x2927;
constexpr uint16_t ColorMuted = 0x8C92;
constexpr uint16_t ColorEvcc = 0x16E9;
constexpr uint16_t ColorEvccDark = 0x0605;
constexpr uint16_t ColorWarn = 0xFEA0;
constexpr uint16_t ColorError = 0xF9C7;

struct EvccState {
  String title = WALLBOX_LABEL;
  String mode = "-";
  String vehicle = "";
  bool connected = false;
  bool charging = false;
  bool enabled = false;
  int vehicleSoc = -1;
  float chargePower = 0;
  float pvPower = 0;
  float gridPower = 0;
  bool evccOk = false;
  String error = "Noch kein EVCC-Status";
  uint32_t lastUpdate = 0;
};

struct AppConfig {
  char evccHost[64] = EVCC_HOST;
  uint16_t evccPort = EVCC_PORT;
  uint8_t loadpointId = EVCC_LOADPOINT_ID;
  char label[32] = WALLBOX_LABEL;
};

struct ModeButton {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  const char *label;
  const char *mode;
  uint16_t color;
};

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TouchCs, TouchIrq);
Preferences preferences;
AppConfig appConfig;
EvccState state;

ModeButton buttons[] = {
    {12, 154, 52, 34, "Aus", "off", ColorError},
    {64, 154, 42, 34, "PV", "pv", ColorEvcc},
    {106, 154, 62, 34, "Min+PV", "minpv", ColorEvccDark},
    {168, 154, 60, 34, "Schnell", "now", ColorWarn},
};

uint32_t lastPoll = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastTouch = 0;
String lastRenderKey;
String lastTopKey;
String lastPowerKey;
String lastModeKey;
String lastVehicleKey;
String lastMetricsKey;
String lastErrorKey;

String evccBaseUrl() {
  return String("http://") + appConfig.evccHost + ":" + String(appConfig.evccPort);
}

String modeLabel(const String &mode) {
  if (mode == "off") {
    return "Aus";
  }
  if (mode == "pv") {
    return "PV";
  }
  if (mode == "minpv") {
    return "Min+PV";
  }
  if (mode == "now") {
    return "Schnell";
  }
  return mode.length() ? mode : "-";
}

String boolLabel(bool value) {
  return value ? "ja" : "nein";
}

String formatPower(float watts) {
  char buffer[16];
  const float absWatts = fabs(watts);
  if (absWatts >= 1000.0f) {
    snprintf(buffer, sizeof(buffer), "%.1f kW", watts / 1000.0f);
  } else {
    snprintf(buffer, sizeof(buffer), "%.0f W", watts);
  }
  return String(buffer);
}

void drawText(int16_t x, int16_t y, const String &text, uint16_t color, uint8_t size = 1, uint16_t bg = ColorBg) {
  tft.setFreeFont(nullptr);
  tft.setTextFont(1);
  tft.setTextColor(color, bg);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(text);
}

void drawGfxText(int16_t x, int16_t y, const String &text, const GFXfont *font, uint16_t color, uint16_t bg = ColorBg,
                 uint8_t datum = TL_DATUM) {
  tft.setTextSize(1);
  tft.setFreeFont(font);
  tft.setTextDatum(datum);
  tft.setTextColor(color, bg, true);
  tft.drawString(text, x, y, GFXFF);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(nullptr);
}

void drawCard(int16_t x, int16_t y, int16_t w, int16_t h) {
  tft.fillRect(x, y, w, h, ColorCard);
  tft.drawRect(x, y, w, h, ColorLine);
}

void drawStatusLine(int16_t y, const String &label, const String &value, uint16_t valueColor = ColorText) {
  tft.setTextSize(1);
  tft.setTextColor(ColorMuted, ColorCard);
  tft.setCursor(24, y);
  tft.print(label);
  tft.setTextColor(valueColor, ColorCard);
  tft.setCursor(130, y);
  tft.print(value);
}

void drawButton(const ModeButton &button) {
  const bool active = state.mode == button.mode;
  const uint16_t fill = active ? ColorEvccDark : ColorCard;
  const uint16_t text = active ? TFT_WHITE : ColorText;

  tft.fillRect(button.x, button.y, button.w, button.h, fill);
  tft.drawRect(button.x, button.y, button.w, button.h, active ? ColorEvccDark : ColorLine);
  drawGfxText(button.x + button.w / 2, button.y + 8, button.label, &FreeSansBold9pt7b, text, fill, TC_DATUM);
}

String renderKey() {
  return state.title + "|" + state.mode + "|" + String(state.connected) + "|" + String(state.charging) + "|" +
         String(static_cast<int>(state.chargePower / 50.0f)) + "|" + String(static_cast<int>(state.pvPower / 100.0f)) +
         "|" + String(static_cast<int>(state.gridPower / 100.0f)) + "|" + String(state.vehicleSoc) + "|" +
         String(state.evccOk) + "|" + state.error;
}

void clearCardArea(int16_t x, int16_t y, int16_t w, int16_t h) {
  tft.fillRect(x, y, w, h, ColorCard);
}

void drawStaticLayout() {
  tft.fillScreen(ColorBg);

  tft.fillRect(0, 56, ScreenW, 2, ColorText);

  drawCard(12, 66, 216, 72);
  tft.drawFastVLine(120, 66, 72, ColorLine);
  drawText(22, 78, "LEISTUNG", ColorMuted, 1, ColorCard);
  drawText(132, 78, "MODUS", ColorMuted, 1, ColorCard);

  tft.drawRect(12, 154, 216, 34, ColorLine);

  drawCard(12, 204, 216, 90);
  tft.drawFastHLine(12, 248, 216, ColorLine);
  tft.drawFastVLine(84, 248, 46, ColorLine);
  tft.drawFastVLine(156, 248, 46, ColorLine);

  tft.fillRect(0, 304, ScreenW, 16, ColorCard);
  tft.drawFastHLine(0, 304, ScreenW, ColorLine);
  tft.fillRect(80, 304, 80, 2, ColorEvcc);
  drawText(102, 310, "Laden", ColorEvcc, 1, ColorCard);
}

void render(bool full = false) {
  const String key = renderKey();
  if (!full && key == lastRenderKey) {
    return;
  }
  lastRenderKey = key;

  if (full) {
    lastTopKey = "";
    lastPowerKey = "";
    lastModeKey = "";
    lastVehicleKey = "";
    lastMetricsKey = "";
    lastErrorKey = "";
    drawStaticLayout();
  }

  const bool wifiOk = WiFi.status() == WL_CONNECTED;

  const String topKey = state.title + "|" + String(wifiOk) + "|" + String(state.evccOk);
  if (full || topKey != lastTopKey) {
    lastTopKey = topKey;
    tft.fillRect(0, 0, ScreenW, 58, ColorBg);
    drawGfxText(12, 9, state.title.substring(0, 10), &FreeSansBold18pt7b, ColorText);
    drawText(14, 43, String("EVCC LP ") + String(appConfig.loadpointId), ColorMuted);
    drawText(178, 43, wifiOk ? (state.evccOk ? "ONLINE" : "WIFI") : "OFFLINE", wifiOk && state.evccOk ? ColorEvccDark : ColorWarn);
  }

  const String powerKey = formatPower(state.chargePower) + "|" + modeLabel(state.mode);
  if (full || powerKey != lastPowerKey) {
    lastPowerKey = powerKey;
    clearCardArea(22, 92, 86, 28);
    drawGfxText(22, 91, formatPower(state.chargePower), &FreeSansBold12pt7b, ColorText, ColorCard);
    clearCardArea(132, 92, 84, 28);
    drawGfxText(132, 93, modeLabel(state.mode), &FreeSans9pt7b, ColorText, ColorCard);
    tft.fillRect(22, 124, 184, 5, ColorCard);
    tft.fillRect(22, 124, 86, 5, ColorEvcc);
    tft.fillRect(108, 124, 98, 5, ColorWarn);
  }

  if (full || state.mode != lastModeKey) {
    lastModeKey = state.mode;
    tft.fillRect(12, 154, 216, 34, ColorCard);
    for (const auto &button : buttons) {
      drawButton(button);
    }
  }

  const String vehicleKey = String(state.connected) + "|" + String(state.charging) + "|" + state.vehicle;
  if (full || vehicleKey != lastVehicleKey) {
    lastVehicleKey = vehicleKey;
    clearCardArea(22, 215, 190, 30);
    drawGfxText(22, 214, state.connected ? "Verbunden" : "Kein Fahrzeug", &FreeSansBold12pt7b, ColorText, ColorCard);
    drawText(22, 238, state.charging ? "LAEDT GERADE" : "NICHT VERBUNDEN", state.charging ? ColorEvccDark : ColorMuted, 1, ColorCard);
  }

  const String errorKey = state.evccOk ? "" : state.error;
  if (full || errorKey != lastErrorKey) {
    if (lastErrorKey.length()) {
      clearCardArea(12, 262, 216, 32);
      lastMetricsKey = "";
    }
    lastErrorKey = errorKey;
  }

  String soc = state.vehicleSoc >= 0 ? String(state.vehicleSoc) + "%" : "-";
  const String metricsKey = formatPower(state.pvPower) + "|" + formatPower(state.gridPower) + "|" + soc;
  if (full || metricsKey != lastMetricsKey) {
    lastMetricsKey = metricsKey;
    clearCardArea(18, 258, 204, 30);
    drawText(24, 258, "PV", ColorMuted, 1, ColorCard);
    drawText(24, 276, formatPower(state.pvPower), ColorEvccDark, 1, ColorCard);
    drawText(96, 258, "NETZ", ColorMuted, 1, ColorCard);
    drawText(96, 276, formatPower(state.gridPower), ColorText, 1, ColorCard);
    drawText(174, 258, "SOC", ColorMuted, 1, ColorCard);
    drawText(174, 276, soc, ColorText, 1, ColorCard);
  }

  if (!state.evccOk && state.error.length()) {
    tft.fillRect(12, 262, 216, 30, ColorError);
    drawText(20, 273, state.error.substring(0, 28), TFT_WHITE, 1, ColorError);
  }
}

void loadAppConfig() {
  preferences.begin("evccswitch", true);
  String host = preferences.getString("evccHost", EVCC_HOST);
  String label = preferences.getString("label", WALLBOX_LABEL);
  appConfig.evccPort = preferences.getUShort("evccPort", EVCC_PORT);
  appConfig.loadpointId = preferences.getUChar("loadpointId", EVCC_LOADPOINT_ID);
  preferences.end();

  host.toCharArray(appConfig.evccHost, sizeof(appConfig.evccHost));
  label.toCharArray(appConfig.label, sizeof(appConfig.label));
  if (appConfig.loadpointId == 0) {
    appConfig.loadpointId = 1;
  }
  state.title = appConfig.label;
}

void saveAppConfig() {
  preferences.begin("evccswitch", false);
  preferences.putString("evccHost", appConfig.evccHost);
  preferences.putUShort("evccPort", appConfig.evccPort);
  preferences.putUChar("loadpointId", appConfig.loadpointId);
  preferences.putString("label", appConfig.label);
  preferences.end();
}

void showSetupPortalInfo() {
  tft.fillScreen(ColorBg);
  drawText(14, 18, "SETUP", ColorText, 2);
  drawCard(12, 58, 216, 172);
  drawText(24, 78, "Mit WLAN verbinden", ColorMuted, 1, ColorCard);
  drawText(24, 100, "EVCC-Switch", ColorText, 2, ColorCard);
  drawText(24, 134, "Passwort", ColorMuted, 1, ColorCard);
  drawText(24, 154, "evccswitch", ColorEvcc, 2, ColorCard);
  drawText(24, 194, "Dann EVCC-IP und", ColorMuted, 1, ColorCard);
  drawText(24, 210, "Loadpoint eintragen.", ColorMuted, 1, ColorCard);
  drawText(14, 286, String("Version ") + APP_VERSION, ColorMuted);
}

void resetConfigurationIfRequested() {
  pinMode(0, INPUT_PULLUP);
  delay(50);
  if (digitalRead(0) == HIGH) {
    return;
  }

  tft.fillScreen(ColorBg);
  drawText(14, 40, "RESET", ColorWarn, 2);
  drawText(14, 80, "Konfiguration", ColorMuted);
  drawText(14, 96, "wird geloescht", ColorMuted);

  WiFiManager wm;
  wm.resetSettings();
  preferences.begin("evccswitch", false);
  preferences.clear();
  preferences.end();
  delay(1000);
  ESP.restart();
}

void setupNetwork() {
  WiFi.mode(WIFI_STA);

  char portBuffer[8];
  char loadpointBuffer[4];
  snprintf(portBuffer, sizeof(portBuffer), "%u", appConfig.evccPort);
  snprintf(loadpointBuffer, sizeof(loadpointBuffer), "%u", appConfig.loadpointId);

  WiFiManagerParameter evccHostParam("evcc_host", "EVCC Host/IP", appConfig.evccHost, sizeof(appConfig.evccHost));
  WiFiManagerParameter evccPortParam("evcc_port", "EVCC Port", portBuffer, sizeof(portBuffer));
  WiFiManagerParameter loadpointParam("loadpoint_id", "EVCC Loadpoint ID", loadpointBuffer, sizeof(loadpointBuffer));
  WiFiManagerParameter labelParam("label", "Display Label", appConfig.label, sizeof(appConfig.label));

  WiFiManager wm;
  wm.setTitle("ESP32 EVCC Switch");
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(20);
  wm.addParameter(&evccHostParam);
  wm.addParameter(&evccPortParam);
  wm.addParameter(&loadpointParam);
  wm.addParameter(&labelParam);

  showSetupPortalInfo();
  const bool connected = wm.autoConnect("EVCC-Switch", "evccswitch");
  if (!connected) {
    ESP.restart();
  }

  strlcpy(appConfig.evccHost, evccHostParam.getValue(), sizeof(appConfig.evccHost));
  strlcpy(appConfig.label, labelParam.getValue(), sizeof(appConfig.label));
  appConfig.evccPort = constrain(atoi(evccPortParam.getValue()), 1, 65535);
  appConfig.loadpointId = constrain(atoi(loadpointParam.getValue()), 1, 9);
  state.title = appConfig.label;
  saveAppConfig();
}

bool parseStatePayload(Stream &payload) {
  JsonDocument filter;
  filter["result"]["loadpoints"][0]["title"] = true;
  filter["result"]["loadpoints"][0]["mode"] = true;
  filter["result"]["loadpoints"][0]["vehicleTitle"] = true;
  filter["result"]["loadpoints"][0]["connected"] = true;
  filter["result"]["loadpoints"][0]["charging"] = true;
  filter["result"]["loadpoints"][0]["enabled"] = true;
  filter["result"]["loadpoints"][0]["vehicleSoc"] = true;
  filter["result"]["loadpoints"][0]["chargePower"] = true;
  filter["result"]["pvPower"] = true;
  filter["result"]["gridPower"] = true;
  filter["result"]["grid"]["power"] = true;
  filter["result"]["site"]["pvPower"] = true;
  filter["loadpoints"][0]["title"] = true;
  filter["loadpoints"][0]["mode"] = true;
  filter["loadpoints"][0]["vehicleTitle"] = true;
  filter["loadpoints"][0]["connected"] = true;
  filter["loadpoints"][0]["charging"] = true;
  filter["loadpoints"][0]["enabled"] = true;
  filter["loadpoints"][0]["vehicleSoc"] = true;
  filter["loadpoints"][0]["chargePower"] = true;
  filter["pvPower"] = true;
  filter["gridPower"] = true;
  filter["grid"]["power"] = true;
  filter["site"]["pvPower"] = true;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    state.error = String("JSON Fehler: ") + error.c_str();
    return false;
  }

  JsonVariant data = doc.as<JsonVariant>();
  if (!data["result"].isNull()) {
    data = data["result"];
  }

  JsonArray loadpoints = data["loadpoints"].as<JsonArray>();
  if (loadpoints.isNull()) {
    state.error = "Keine loadpoints in /api/state";
    return false;
  }

  const int index = appConfig.loadpointId - 1;
  if (index < 0 || index >= static_cast<int>(loadpoints.size())) {
    state.error = "Loadpoint-ID nicht gefunden";
    return false;
  }

  JsonVariant loadpoint = loadpoints[index];
  state.title = loadpoint["title"] | appConfig.label;
  state.mode = loadpoint["mode"] | "-";
  state.vehicle = loadpoint["vehicleTitle"] | "";
  state.connected = loadpoint["connected"] | false;
  state.charging = loadpoint["charging"] | false;
  state.enabled = loadpoint["enabled"] | false;
  state.vehicleSoc = loadpoint["vehicleSoc"] | -1;
  state.chargePower = loadpoint["chargePower"] | 0.0f;
  state.pvPower = data["pvPower"] | (data["site"]["pvPower"] | 0.0f);
  state.gridPower = data["gridPower"] | (data["grid"]["power"] | 0.0f);
  state.evccOk = true;
  state.error = "";
  state.lastUpdate = millis();
  return true;
}

bool fetchEvccState() {
  if (WiFi.status() != WL_CONNECTED) {
    state.evccOk = false;
    state.error = "WLAN nicht verbunden";
    return false;
  }

  HTTPClient http;
  http.setTimeout(EvccTimeoutMs);
  http.setReuse(false);
  const String url = evccBaseUrl() + "/api/state";
  if (!http.begin(url)) {
    state.evccOk = false;
    state.error = "EVCC URL ungueltig";
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.useHTTP10(true);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    state.evccOk = false;
    state.error = "EVCC HTTP " + String(statusCode);
    http.end();
    return false;
  }

  WiFiClient &payload = http.getStream();
  const bool ok = parseStatePayload(payload);
  http.end();
  state.evccOk = ok;
  return ok;
}

bool setEvccMode(const char *mode) {
  if (WiFi.status() != WL_CONNECTED) {
    state.error = "WLAN nicht verbunden";
    state.evccOk = false;
    return false;
  }

  HTTPClient http;
  http.setTimeout(EvccTimeoutMs);
  http.setReuse(false);
  const String url = evccBaseUrl() + "/api/loadpoints/" + String(appConfig.loadpointId) + "/mode/" + mode;
  if (!http.begin(url)) {
    state.error = "EVCC URL ungueltig";
    return false;
  }

  const int statusCode = http.POST("");
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    state.error = "Setzen fehlgeschlagen: " + String(statusCode);
    state.evccOk = false;
    return false;
  }

  state.mode = mode;
  state.error = "";
  return true;
}

bool readTouchPoint(int16_t &x, int16_t &y) {
  if (!touch.touched()) {
    return false;
  }

  TS_Point point = touch.getPoint();
  x = map(point.x, TouchMinX, TouchMaxX, 0, ScreenW);
  y = map(point.y, TouchMinY, TouchMaxY, 0, ScreenH);
  x = constrain(x, 0, ScreenW - 1);
  y = constrain(y, 0, ScreenH - 1);
  return true;
}

void handleTouch() {
  const uint32_t now = millis();
  if (now - lastTouch < TouchDebounceMs) {
    return;
  }

  int16_t x;
  int16_t y;
  if (!readTouchPoint(x, y)) {
    return;
  }

  lastTouch = now;
  for (const auto &button : buttons) {
    const bool inside = x >= button.x && x <= button.x + button.w && y >= button.y && y <= button.y + button.h;
    if (!inside) {
      continue;
    }

    tft.fillRect(12, 262, 216, 30, ColorWarn);
    drawText(20, 273, String("Setze Modus: ") + button.label, ColorText, 1, ColorWarn);
    setEvccMode(button.mode);
    fetchEvccState();
    render(true);
    return;
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(ColorBg);

  touchSpi.begin(TouchClk, TouchMiso, TouchMosi, TouchCs);
  touch.begin(touchSpi);
  touch.setRotation(TOUCH_ROTATION);

  loadAppConfig();
  resetConfigurationIfRequested();
  setupNetwork();
  fetchEvccState();
  render(true);
}

void loop() {
  handleTouch();

  if (WiFi.status() != WL_CONNECTED) {
    const uint32_t now = millis();
    if (now - lastWifiAttempt > WifiRetryMs) {
      lastWifiAttempt = now;
      WiFi.reconnect();
    }
  }

  const uint32_t now = millis();
  if (now - lastPoll >= PollIntervalMs) {
    lastPoll = now;
    fetchEvccState();
    render();
  }

  delay(20);
}
