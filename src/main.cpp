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
#ifndef EVCC_STANDARD_MODE
#define EVCC_STANDARD_MODE "minpv"
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
constexpr uint8_t MaxSilentFetchFailures = 2;

const char EvccStateQuery[] =
    "/api/state?jq=%7Bloadpoints%3A%5B.loadpoints%5B%5D%7C%7Btitle%2Cmode%2CvehicleTitle%2Cconnected%2Ccharging%2Cenabled%2CvehicleSoc%2CchargePower%7D%5D%2CpvPower%2CgridPower%2Cgrid%3A%7Bpower%3A.grid.power%7D%2Csite%3A%7BpvPower%3A.site.pvPower%7D%7D";

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
  char standardMode[8] = EVCC_STANDARD_MODE;
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
    {12, 106, 216, 50, "STANDARD", "std", ColorEvccDark},
    {12, 166, 104, 50, "AUS", "off", ColorError},
    {124, 166, 104, 50, "SCHNELL", "now", ColorWarn},
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
bool lastConnected = false;
bool connectionStateKnown = false;
uint8_t consecutiveFetchFailures = 0;

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

bool isStandardMode(const String &mode) {
  return mode == "pv" || mode == "minpv";
}

void setStandardModeValue(const char *value) {
  if (strcmp(value, "pv") == 0) {
    strlcpy(appConfig.standardMode, "pv", sizeof(appConfig.standardMode));
    return;
  }
  strlcpy(appConfig.standardMode, "minpv", sizeof(appConfig.standardMode));
}

const char *resolveMode(const char *mode) {
  if (strcmp(mode, "std") == 0) {
    return appConfig.standardMode;
  }
  return mode;
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

void drawFrame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color = ColorLine) {
  tft.drawRect(x, y, w, h, color);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, color);
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
  const bool standardButton = strcmp(button.mode, "std") == 0;
  const bool active = standardButton ? isStandardMode(state.mode) : state.mode == button.mode;
  const uint16_t fill = active ? ColorEvccDark : ColorCard;
  const uint16_t text = active ? TFT_WHITE : ColorText;
  const uint16_t frame = active ? ColorEvccDark : ColorLine;

  tft.fillRect(button.x, button.y, button.w, button.h, fill);
  drawFrame(button.x, button.y, button.w, button.h, frame);
  drawGfxText(button.x + button.w / 2, button.y + button.h / 2 + 1, button.label, &FreeSansBold9pt7b, text, fill,
              MC_DATUM);
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

  tft.fillRect(0, 46, ScreenW, 2, ColorText);

  drawCard(12, 56, 216, 34);
  tft.drawFastVLine(84, 56, 34, ColorLine);
  tft.drawFastVLine(156, 56, 34, ColorLine);

  drawText(12, 94, "LADEMODUS", ColorMuted);

  drawCard(12, 226, 216, 68);
  tft.drawFastHLine(12, 258, 216, ColorLine);
  tft.drawFastVLine(84, 258, 36, ColorLine);
  tft.drawFastVLine(156, 258, 36, ColorLine);

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
    tft.fillRect(0, 0, ScreenW, 46, ColorBg);
    drawGfxText(12, 8, state.title.substring(0, 11), &FreeSansBold12pt7b, ColorText);
    drawText(14, 34, String("LP ") + String(appConfig.loadpointId), ColorMuted);
    drawText(178, 34, wifiOk ? (state.evccOk ? "ONLINE" : "WIFI") : "OFFLINE", wifiOk && state.evccOk ? ColorEvccDark : ColorWarn);
  }

  const String powerKey = formatPower(state.chargePower) + "|" + modeLabel(state.mode) + "|" + String(state.connected) + "|" +
                          String(state.charging);
  if (full || powerKey != lastPowerKey) {
    lastPowerKey = powerKey;
    clearCardArea(14, 58, 212, 30);
    drawText(22, 62, "POWER", ColorMuted, 1, ColorCard);
    drawText(22, 76, formatPower(state.chargePower), ColorText, 1, ColorCard);
    drawText(94, 62, "MODE", ColorMuted, 1, ColorCard);
    drawText(94, 76, isStandardMode(state.mode) ? "Standard" : modeLabel(state.mode), ColorText, 1, ColorCard);
    drawText(166, 62, "AUTO", ColorMuted, 1, ColorCard);
    const String carState = state.connected ? (state.charging ? "LOAD" : "READY") : "OFF";
    drawText(166, 76, carState, state.connected ? ColorEvccDark : ColorMuted, 1, ColorCard);
    drawFrame(12, 56, 216, 34);
    tft.drawFastVLine(84, 56, 34, ColorLine);
    tft.drawFastVLine(156, 56, 34, ColorLine);
  }

  if (full || state.mode != lastModeKey) {
    lastModeKey = state.mode;
    tft.fillRect(12, 104, 216, 112, ColorBg);
    for (const auto &button : buttons) {
      drawButton(button);
    }
  }

  const String vehicleKey = String(state.connected) + "|" + String(state.charging) + "|" + state.vehicle;
  if (full || vehicleKey != lastVehicleKey) {
    lastVehicleKey = vehicleKey;
    clearCardArea(18, 234, 204, 20);
    const String status = state.connected ? (state.charging ? "LAEDT" : "VERBUNDEN") : "KEIN FAHRZEUG";
    const String detail = state.connected ? (state.vehicle.length() ? state.vehicle : "BEREIT") : "NICHT VERBUNDEN";
    drawText(22, 234, status, state.connected ? ColorEvccDark : ColorText, 1, ColorCard);
    drawText(22, 246, detail.substring(0, 24), ColorMuted, 1, ColorCard);
    drawFrame(12, 226, 216, 68);
    tft.drawFastHLine(12, 258, 216, ColorLine);
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
    clearCardArea(18, 264, 204, 24);
    drawText(24, 264, "PV", ColorMuted, 1, ColorCard);
    drawText(24, 278, formatPower(state.pvPower), ColorEvccDark, 1, ColorCard);
    drawText(96, 264, "NETZ", ColorMuted, 1, ColorCard);
    drawText(96, 278, formatPower(state.gridPower), ColorText, 1, ColorCard);
    drawText(174, 264, "SOC", ColorMuted, 1, ColorCard);
    drawText(174, 278, soc, ColorText, 1, ColorCard);
    drawFrame(12, 226, 216, 68);
    tft.drawFastHLine(12, 258, 216, ColorLine);
    tft.drawFastVLine(84, 258, 36, ColorLine);
    tft.drawFastVLine(156, 258, 36, ColorLine);
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
  String standardMode = preferences.getString("standard", EVCC_STANDARD_MODE);
  appConfig.evccPort = preferences.getUShort("evccPort", EVCC_PORT);
  appConfig.loadpointId = preferences.getUChar("loadpointId", EVCC_LOADPOINT_ID);
  preferences.end();

  host.toCharArray(appConfig.evccHost, sizeof(appConfig.evccHost));
  label.toCharArray(appConfig.label, sizeof(appConfig.label));
  setStandardModeValue(standardMode.c_str());
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
  preferences.putString("standard", appConfig.standardMode);
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
  char standardModeBuffer[8];
  strlcpy(standardModeBuffer, appConfig.standardMode, sizeof(standardModeBuffer));

  WiFiManagerParameter evccHostParam("evcc_host", "EVCC Host/IP", appConfig.evccHost, sizeof(appConfig.evccHost));
  WiFiManagerParameter evccPortParam("evcc_port", "EVCC Port", portBuffer, sizeof(portBuffer));
  WiFiManagerParameter loadpointParam("loadpoint_id", "EVCC Loadpoint ID", loadpointBuffer, sizeof(loadpointBuffer));
  WiFiManagerParameter labelParam("label", "Display Label", appConfig.label, sizeof(appConfig.label));
  WiFiManagerParameter standardModeParam("standard_mode", "Standard mode: pv or minpv", standardModeBuffer,
                                         sizeof(standardModeBuffer));

  WiFiManager wm;
  wm.setTitle("ESP32 EVCC Switch");
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(20);
  wm.addParameter(&evccHostParam);
  wm.addParameter(&evccPortParam);
  wm.addParameter(&loadpointParam);
  wm.addParameter(&labelParam);
  wm.addParameter(&standardModeParam);

  showSetupPortalInfo();
  const bool connected = wm.autoConnect("EVCC-Switch", "evccswitch");
  if (!connected) {
    ESP.restart();
  }

  strlcpy(appConfig.evccHost, evccHostParam.getValue(), sizeof(appConfig.evccHost));
  strlcpy(appConfig.label, labelParam.getValue(), sizeof(appConfig.label));
  setStandardModeValue(standardModeParam.getValue());
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
    state.error = String("JSON ERROR: ") + error.c_str();
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
  consecutiveFetchFailures = 0;
  state.lastUpdate = millis();
  return true;
}

void markFetchFailure(const String &message) {
  consecutiveFetchFailures++;
  if (consecutiveFetchFailures >= MaxSilentFetchFailures || state.lastUpdate == 0) {
    state.evccOk = false;
    state.error = message;
  }
}

bool fetchEvccState() {
  if (WiFi.status() != WL_CONNECTED) {
    markFetchFailure("WLAN OFFLINE");
    return false;
  }

  HTTPClient http;
  http.setTimeout(EvccTimeoutMs);
  http.setReuse(false);
  const String url = evccBaseUrl() + EvccStateQuery;
  if (!http.begin(url)) {
    markFetchFailure("EVCC URL ERROR");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.useHTTP10(true);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    markFetchFailure("EVCC HTTP " + String(statusCode));
    http.end();
    return false;
  }

  WiFiClient &payload = http.getStream();
  const bool ok = parseStatePayload(payload);
  http.end();
  if (!ok) {
    markFetchFailure(state.error);
  }
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

void updateConnectionReset() {
  if (!state.evccOk) {
    return;
  }

  if (!connectionStateKnown) {
    connectionStateKnown = true;
    lastConnected = state.connected;
    return;
  }

  const bool disconnectedAfterSession = lastConnected && !state.connected;
  lastConnected = state.connected;

  if (disconnectedAfterSession && !isStandardMode(state.mode)) {
    setEvccMode(appConfig.standardMode);
  }
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
    drawText(20, 273, String("SET ") + button.label, ColorText, 1, ColorWarn);
    if (setEvccMode(resolveMode(button.mode))) {
      lastPoll = millis();
    }
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
  updateConnectionReset();
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
    updateConnectionReset();
    render();
  }

  delay(20);
}
