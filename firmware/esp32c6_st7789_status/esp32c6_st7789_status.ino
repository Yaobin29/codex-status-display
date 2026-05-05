#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#define WIFI_STATUS_ENABLED 1
#else
#define WIFI_STATUS_ENABLED 0
#endif

// Board/display settings recovered from the MagnoDLP ESP32-C6 LVGL sketch.
#define TFT_CS 14
#define TFT_RST 21
#define TFT_DC 15
#define TFT_MOSI 6
#define TFT_SCLK 7
#define TFT_BACKLIGHT 22
#define TFT_WIDTH 172
#define TFT_HEIGHT 320
#define TFT_ROTATION 0
#define SERIAL_BAUD 115200
#define MAX_LINE_BYTES 1400
#define WIFI_POLL_MS 5000
#define WIFI_RETRY_MS 12000
#define PROGRESS_PERIOD_MS 3600
#define PROGRESS_UPDATE_MS 90
#define LED_BREATH_PERIOD_MS 2600
#define LED_COLOR_PERIOD_MS 5200
#define LED_MAX_BRIGHTNESS 96
#define LED_MIN_INTENSITY 4

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

String inputLine;
unsigned long lastPacketAt = 0;
bool hasPacket = false;
unsigned long lastAnimAt = 0;
uint8_t animMode = 0;
int lastRunningProjects = 0;
int lastAwaitingResponse = 0;
int lastDoneToday = 0;
unsigned long lastWifiPollAt = 0;
unsigned long lastWifiAttemptAt = 0;
bool lastPacketFromWifi = false;
uint8_t lastBreathLevel = 0;
int lastProgressFillWidth = -1;
uint16_t lastProgressColor = 0;
bool ledWasOn = false;

const int TOP_Y = 0;
const int TOP_H = 34;
const int METRIC_Y = 42;
const int METRIC_H = 74;
const int METRIC_W = 79;
const int METRIC_GAP = 6;
const int PROGRESS_X = 8;
const int PROGRESS_Y = 124;
const int PROGRESS_W = 156;
const int PROGRESS_H = 10;
const int LIST_TITLE_Y = 142;
const int LIST_Y = 166;
const int LIST_ROW_H = 30;
const int LIST_ROWS = 4;
const int FOOTER_Y = 302;
const int FOOTER_H = 18;

struct DisplaySnapshot {
  int runningProjects = -1;
  int doneToday = -1;
  int awaitingResponse = -1;
  int projectRows = 0;
  uint8_t mode = 255;
  bool wifi = false;
  String minute;
  String projectName[LIST_ROWS];
  String projectStatus[LIST_ROWS];
};

DisplaySnapshot lastSnapshot;
bool uiInitialized = false;
bool staleShown = false;

uint16_t colorForUrgent(int urgent) {
  if (urgent > 0) {
    return ST77XX_RED;
  }
  return ST77XX_GREEN;
}

String asciiDisplayText(const String &raw, const String &fallback, int maxLen) {
  String out;
  out.reserve(maxLen + 1);
  bool lastWasSpace = false;
  for (size_t i = 0; i < raw.length() && out.length() < (size_t)maxLen; i++) {
    char c = raw[i];
    if (c >= 32 && c <= 126) {
      if (c == ' ' || c == '\t') {
        if (!lastWasSpace && out.length() > 0) {
          out += ' ';
          lastWasSpace = true;
        }
      } else {
        out += c;
        lastWasSpace = false;
      }
    }
  }
  out.trim();
  if (out.length() == 0) {
    return fallback;
  }
  return out;
}

String shortTime(const char *isoTime) {
  String value = isoTime ? String(isoTime) : String("");
  int tIndex = value.indexOf('T');
  if (tIndex >= 0 && value.length() >= (size_t)(tIndex + 6)) {
    return value.substring(tIndex + 1, tIndex + 6);
  }
  return "--:--";
}

void printWrapped(const String &text, int x, int y, int maxChars, int maxLines, uint16_t color) {
  tft.setTextColor(color);
  tft.setTextSize(1);
  String clean = text;
  clean.trim();
  int line = 0;
  while (clean.length() > 0 && line < maxLines) {
    int take = min(maxChars, (int)clean.length());
    int breakAt = -1;
    for (int i = 0; i < take; i++) {
      if (clean[i] == ' ') {
        breakAt = i;
      }
    }
    if ((int)clean.length() > maxChars && breakAt > 4) {
      take = breakAt;
    }
    String chunk = clean.substring(0, take);
    chunk.trim();
    tft.setCursor(x, y + line * 10);
    tft.print(chunk);
    clean = clean.substring(take);
    clean.trim();
    line++;
  }
}

void drawBadge(int x, int y, const char *label, int value, uint16_t color) {
  tft.drawRoundRect(x, y, 61, 38, 4, color);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(x + 4, y + 5);
  tft.print(label);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(x + 4, y + 18);
  tft.print(value);
}

String countText(int value) {
  if (value > 99) {
    return "99+";
  }
  return String(value);
}

String arrayFieldAt(JsonDocument &doc, const char *arrayName, int index, const char *fieldName, const String &fallback, int maxLen) {
  JsonArray items = doc[arrayName].as<JsonArray>();
  if (!items.isNull() && items.size() > (size_t)index) {
    String value = asciiDisplayText(items[index][fieldName] | "", fallback, maxLen);
    if (value.length() > 0) {
      return value;
    }
  }
  return fallback;
}

String arrayField(JsonDocument &doc, const char *arrayName, const char *fieldName, const String &fallback, int maxLen) {
  return arrayFieldAt(doc, arrayName, 0, fieldName, fallback, maxLen);
}

String compactTitle(JsonDocument &doc, const char *arrayName, int index, const String &fallback) {
  String value = arrayFieldAt(doc, arrayName, index, "title", fallback, 10);
  if (value.length() >= 10) {
    value = value.substring(0, 8) + "..";
  }
  return value;
}

String compactNameAt(JsonDocument &doc, const char *arrayName, int index, const String &fallback) {
  String value = arrayFieldAt(doc, arrayName, index, "name", fallback, 10);
  if (value.length() >= 10) {
    value = value.substring(0, 8) + "..";
  }
  return value;
}

String statusAt(JsonDocument &doc, const char *arrayName, int index) {
  return arrayFieldAt(doc, arrayName, index, "status", "", 24);
}

uint16_t modeColor(bool dim) {
  if (animMode == 3) {
    return dim ? tft.color565(64, 0, 0) : ST77XX_RED;
  }
  if (animMode == 2) {
    return dim ? tft.color565(58, 48, 0) : ST77XX_YELLOW;
  }
  if (animMode == 1) {
    return dim ? tft.color565(0, 42, 58) : ST77XX_CYAN;
  }
  return dim ? tft.color565(0, 48, 18) : ST77XX_GREEN;
}

uint8_t breathLevel(unsigned long now) {
  uint16_t phase = now % LED_BREATH_PERIOD_MS;
  uint16_t half = LED_BREATH_PERIOD_MS / 2;
  uint16_t ramp = phase < half ? phase : LED_BREATH_PERIOD_MS - phase;
  uint8_t linear = (uint32_t)ramp * 100 / half;
  uint8_t eased;
  if (linear < 50) {
    eased = (uint16_t)linear * linear / 50;
  } else {
    uint8_t inv = 100 - linear;
    eased = 100 - (uint16_t)inv * inv / 50;
  }
  return LED_MIN_INTENSITY + (uint16_t)eased * (100 - LED_MIN_INTENSITY) / 100;
}

uint8_t progressLevel(unsigned long now) {
  return min(100, (int)((uint32_t)(now % PROGRESS_PERIOD_MS) * 101 / PROGRESS_PERIOD_MS));
}

uint8_t colorBlendLevel(unsigned long now) {
  uint16_t phase = now % LED_COLOR_PERIOD_MS;
  uint16_t half = LED_COLOR_PERIOD_MS / 2;
  uint16_t ramp = phase < half ? phase : LED_COLOR_PERIOD_MS - phase;
  return (uint32_t)ramp * 100 / half;
}

uint8_t mixChannel(uint8_t a, uint8_t b, uint8_t amount) {
  return ((uint16_t)a * (100 - amount) + (uint16_t)b * amount) / 100;
}

void statusRgb(int runningProjects, int awaitingResponse, int doneToday, uint8_t intensity, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t baseR = 28;
  uint8_t baseG = 28;
  uint8_t baseB = 28;
  if (awaitingResponse > 0) {
    baseR = 255;
    baseG = 146;
    baseB = 20;
  } else if (runningProjects > 0) {
    baseR = 48;
    baseG = 230;
    baseB = 85;
  } else if (doneToday > 0) {
    baseR = 0;
    baseG = 190;
    baseB = 220;
  }
  r = (uint16_t)baseR * intensity / 100;
  g = (uint16_t)baseG * intensity / 100;
  b = (uint16_t)baseB * intensity / 100;
}

uint16_t statusColor(int runningProjects, int awaitingResponse, int doneToday, uint8_t intensity) {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  statusRgb(runningProjects, awaitingResponse, doneToday, intensity, r, g, b);
  return tft.color565(r, g, b);
}

void statusLedRgb(int runningProjects, int awaitingResponse, uint8_t intensity, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t blend = colorBlendLevel(millis());
  uint8_t baseR = 0;
  uint8_t baseG = 0;
  uint8_t baseB = 0;
  uint8_t altR = 0;
  uint8_t altG = 0;
  uint8_t altB = 0;

  if (awaitingResponse > 0) {
    baseR = 255;
    baseG = 132;
    baseB = 8;
    altR = 255;
    altG = 34;
    altB = 0;
  } else {
    baseR = 36;
    baseG = 235;
    baseB = 76;
    altR = 0;
    altG = 170;
    altB = 255;
  }

  r = (uint16_t)mixChannel(baseR, altR, blend) * intensity / 100;
  g = (uint16_t)mixChannel(baseG, altG, blend) * intensity / 100;
  b = (uint16_t)mixChannel(baseB, altB, blend) * intensity / 100;
}

void writeStatusLed(uint8_t intensity, int runningProjects, int awaitingResponse) {
#if defined(RGB_BUILTIN)
  if (runningProjects <= 0 && awaitingResponse <= 0) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    ledWasOn = false;
    lastBreathLevel = 0;
    return;
  }
  uint8_t r;
  uint8_t g;
  uint8_t b;
  statusLedRgb(runningProjects, awaitingResponse, intensity, r, g, b);
  uint8_t ledR = (uint16_t)r * LED_MAX_BRIGHTNESS / 255;
  uint8_t ledG = (uint16_t)g * LED_MAX_BRIGHTNESS / 255;
  uint8_t ledB = (uint16_t)b * LED_MAX_BRIGHTNESS / 255;
  rgbLedWrite(RGB_BUILTIN, ledR, ledG, ledB);
  ledWasOn = true;
#endif
}

bool wifiConnected() {
#if WIFI_STATUS_ENABLED
  return WiFi.status() == WL_CONNECTED;
#else
  return false;
#endif
}

uint16_t runningColor(int runningProjects, int awaitingResponse, bool dim) {
  return statusColor(runningProjects, awaitingResponse, 0, dim ? 28 : 100);
}

uint16_t rowColor(const String &status) {
  if (status == "awaiting_response") {
    return tft.color565(218, 74, 16);
  }
  return tft.color565(71, 230, 92);
}

void drawWifiIcon(int x, int y, bool connected) {
  uint16_t color = connected ? ST77XX_CYAN : tft.color565(190, 55, 35);
  tft.drawLine(x + 1, y + 5, x + 8, y, color);
  tft.drawLine(x + 8, y, x + 15, y + 5, color);
  tft.drawLine(x + 4, y + 9, x + 8, y + 6, color);
  tft.drawLine(x + 8, y + 6, x + 12, y + 9, color);
  tft.fillCircle(x + 8, y + 13, 2, color);
}

void drawWifiStatus(bool connected) {
  tft.fillRect(4, 5, 24, 24, ST77XX_BLACK);
  drawWifiIcon(7, 9, connected);
}

void drawHeaderTime(const String &minute) {
  tft.fillRect(104, 4, 64, 24, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(106, 7);
  tft.print(minute);
}

void drawHeaderBase(const DisplaySnapshot &snapshot) {
  tft.fillRect(0, TOP_Y, TFT_WIDTH, TOP_H, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(32, 7);
  tft.print("Codex");
  tft.drawFastHLine(4, TOP_H - 1, TFT_WIDTH - 8, tft.color565(24, 62, 70));
  drawWifiStatus(snapshot.wifi);
  drawHeaderTime(snapshot.minute);
}

void drawMetricFrame(int x, const char *label, uint16_t color) {
  tft.fillRect(x, METRIC_Y, METRIC_W, METRIC_H, ST77XX_BLACK);
  tft.drawRoundRect(x, METRIC_Y, METRIC_W, METRIC_H, 6, color);
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setTextSize(2);
  int labelW = strlen(label) * 12;
  tft.setCursor(x + max(6, (METRIC_W - labelW) / 2), METRIC_Y + 7);
  tft.print(label);
}

void drawMetricValue(int x, int value, uint16_t color) {
  tft.fillRect(x + 6, METRIC_Y + 31, METRIC_W - 12, 38, ST77XX_BLACK);
  String valueText = countText(value);
  int valueSize = valueText.length() > 2 ? 3 : 4;
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setTextSize(valueSize);
  int textW = valueText.length() * 6 * valueSize;
  int cursorX = x + max(6, (METRIC_W - textW) / 2);
  int cursorY = METRIC_Y + (valueSize == 4 ? 34 : 38);
  tft.setCursor(cursorX, cursorY);
  tft.print(valueText);
}

void drawMetricPanel(int x, const char *label, int value, uint16_t color) {
  drawMetricFrame(x, label, color);
  drawMetricValue(x, value, color);
}

void updateMetricPanel(int x, const char *label, int value, int lastValue, uint16_t color, uint16_t lastColor, bool force) {
  bool colorChanged = color != lastColor;
  if (force || colorChanged) {
    drawMetricPanel(x, label, value, color);
    return;
  }
  if (value != lastValue) {
    drawMetricValue(x, value, color);
  }
}

void updateMetrics(const DisplaySnapshot &snapshot, bool force) {
  uint16_t runColor = runningColor(snapshot.runningProjects, snapshot.awaitingResponse, false);
  uint16_t doneColor = snapshot.doneToday > 0 ? ST77XX_CYAN : tft.color565(78, 78, 78);
  uint16_t lastRunColor = runningColor(lastSnapshot.runningProjects, lastSnapshot.awaitingResponse, false);
  uint16_t lastDoneColor = lastSnapshot.doneToday > 0 ? ST77XX_CYAN : tft.color565(78, 78, 78);
  updateMetricPanel(4, "Run", snapshot.runningProjects, lastSnapshot.runningProjects, runColor, lastRunColor, force);
  updateMetricPanel(4 + METRIC_W + METRIC_GAP, "Done", snapshot.doneToday, lastSnapshot.doneToday, doneColor, lastDoneColor, force);

  bool progressStateChanged =
    force ||
    snapshot.runningProjects != lastSnapshot.runningProjects ||
    snapshot.awaitingResponse != lastSnapshot.awaitingResponse ||
    (snapshot.doneToday > 0) != (lastSnapshot.doneToday > 0);
  if (progressStateChanged) {
    lastProgressFillWidth = -1;
  }
  unsigned long now = millis();
  drawRunProgress(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, snapshot.runningProjects, snapshot.awaitingResponse, snapshot.doneToday, progressLevel(now), progressStateChanged);
  lastBreathLevel = breathLevel(now);
  writeStatusLed(lastBreathLevel, snapshot.runningProjects, snapshot.awaitingResponse);
}

void drawRunProgress(int x, int y, int w, int h, int runningProjects, int awaitingResponse, int doneToday, uint8_t progress, bool force) {
  bool active = runningProjects > 0 || awaitingResponse > 0;
  uint16_t color = statusColor(runningProjects, awaitingResponse, doneToday, active ? 100 : (doneToday > 0 ? 32 : 0));
  uint16_t dim = statusColor(runningProjects, awaitingResponse, doneToday, doneToday > 0 ? 22 : 12);
  int innerW = w - 4;
  int fillW = active ? max(2, (int)((uint32_t)innerW * progress / 100)) : (doneToday > 0 ? innerW : 0);
  bool reset = force || lastProgressFillWidth < 0 || fillW < lastProgressFillWidth || color != lastProgressColor;
  if (reset) {
    tft.fillRect(x, y, w, h, ST77XX_BLACK);
    tft.drawRect(x, y, w, h, dim);
    lastProgressFillWidth = 0;
    lastProgressColor = color;
  }
  if (!active && doneToday <= 0) {
    return;
  }
  if (fillW > lastProgressFillWidth) {
    tft.fillRect(x + 2 + lastProgressFillWidth, y + 2, fillW - lastProgressFillWidth, h - 4, color);
    lastProgressFillWidth = fillW;
  }
}

void updateAnimation() {
  if (!uiInitialized) {
    return;
  }
  bool active = lastRunningProjects > 0 || lastAwaitingResponse > 0;
  if (!active) {
    if (ledWasOn) {
      writeStatusLed(0, lastRunningProjects, lastAwaitingResponse);
    }
    return;
  }
  unsigned long now = millis();
  if (now - lastAnimAt < PROGRESS_UPDATE_MS) {
    return;
  }
  lastAnimAt = now;
  drawRunProgress(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, lastRunningProjects, lastAwaitingResponse, lastDoneToday, progressLevel(now), false);
  lastBreathLevel = breathLevel(now);
  writeStatusLed(lastBreathLevel, lastRunningProjects, lastAwaitingResponse);
}

void drawChatRow(int index, const String &name, const String &status) {
  int y = LIST_Y + index * LIST_ROW_H;
  uint16_t color = rowColor(status);
  tft.fillRect(4, y, TFT_WIDTH - 8, LIST_ROW_H - 2, ST77XX_BLACK);
  tft.fillRoundRect(9, y + 7, 13, 13, 4, color);
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, y + 3);
  tft.print(name);
  if (status == "awaiting_response") {
    tft.setTextColor(tft.color565(255, 170, 30), ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(134, y + 10);
    tft.print("RESP");
  }
}

void clearChatRow(int index) {
  int y = LIST_Y + index * LIST_ROW_H;
  tft.fillRect(4, y, TFT_WIDTH - 8, LIST_ROW_H - 2, ST77XX_BLACK);
}

void drawProjectHeader() {
  tft.fillRect(0, LIST_TITLE_Y, TFT_WIDTH, LIST_Y - LIST_TITLE_Y, ST77XX_BLACK);
  tft.setTextColor(tft.color565(140, 200, 205), ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, LIST_TITLE_Y);
  tft.print("Active");
}

void drawIdleState() {
  tft.setTextColor(tft.color565(110, 110, 110), ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(42, LIST_Y + 28);
  tft.print("idle");
}

void updateProjectList(const DisplaySnapshot &snapshot, bool force) {
  if (force) {
    tft.fillRect(0, LIST_TITLE_Y, TFT_WIDTH, FOOTER_Y - LIST_TITLE_Y, ST77XX_BLACK);
    drawProjectHeader();
  }

  if (force || (snapshot.projectRows == 0) != (lastSnapshot.projectRows == 0)) {
    tft.fillRect(0, LIST_Y, TFT_WIDTH, FOOTER_Y - LIST_Y, ST77XX_BLACK);
  }
  if (snapshot.projectRows <= 0) {
    if (force || lastSnapshot.projectRows > 0) {
      drawIdleState();
    }
    return;
  }

  for (int i = 0; i < LIST_ROWS; i++) {
    if (i < snapshot.projectRows) {
      bool rowChanged =
        force ||
        i >= lastSnapshot.projectRows ||
        snapshot.projectName[i] != lastSnapshot.projectName[i] ||
        snapshot.projectStatus[i] != lastSnapshot.projectStatus[i];
      if (rowChanged) {
        drawChatRow(i, snapshot.projectName[i], snapshot.projectStatus[i]);
      }
    } else if (i < lastSnapshot.projectRows) {
      clearChatRow(i);
    }
  }
}

void drawFooter(const char *message, uint16_t color) {
  tft.fillRect(0, FOOTER_Y, TFT_WIDTH, FOOTER_H, ST77XX_BLACK);
  if (!message || !message[0]) {
    return;
  }
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(6, FOOTER_Y + 5);
  tft.print(message);
}

void clearFooter() {
  drawFooter("", ST77XX_WHITE);
  staleShown = false;
}

void drawWaiting() {
  tft.fillScreen(ST77XX_BLACK);
  uiInitialized = false;
  staleShown = false;
  writeStatusLed(0, 0, 0);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 24);
  tft.print("CODEX");
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 58);
  tft.print("Waiting for Codex JSON");
  tft.setCursor(8, 76);
  tft.print("WiFi or 115200 serial");
}

uint8_t snapshotMode(int awaitingResponse, int doneToday, int runningProjects) {
  if (awaitingResponse > 0) {
    return 2;
  }
  if (doneToday > 0) {
    return 1;
  }
  if (runningProjects > 0) {
    return 0;
  }
  return 3;
}

String compactProjectName(JsonObject item, const String &fallback, int maxLen) {
  String value = asciiDisplayText(item["name"] | item["title"] | "", fallback, maxLen);
  if (value.length() > maxLen - 1) {
    value = value.substring(0, max(1, maxLen - 3)) + "..";
  }
  return value;
}

bool sameProjects(const DisplaySnapshot &a, const DisplaySnapshot &b) {
  if (a.projectRows != b.projectRows) {
    return false;
  }
  for (int i = 0; i < LIST_ROWS; i++) {
    if (a.projectName[i] != b.projectName[i] || a.projectStatus[i] != b.projectStatus[i]) {
      return false;
    }
  }
  return true;
}

DisplaySnapshot buildSnapshot(JsonDocument &doc) {
  DisplaySnapshot snapshot;
  JsonObject counts = doc["counts"];
  snapshot.awaitingResponse = counts["awaiting_response"] | 0;
  snapshot.runningProjects = counts["running_projects"] | 0;
  snapshot.doneToday = counts["completed_today"] | 0;
  if (snapshot.doneToday == 0 && counts.containsKey("done_unseen")) {
    snapshot.doneToday = counts["done_unseen"] | 0;
  }
  snapshot.minute = shortTime(doc["t"] | "");
  snapshot.wifi = wifiConnected();
  snapshot.mode = snapshotMode(snapshot.awaitingResponse, snapshot.doneToday, snapshot.runningProjects);

  JsonArray projects = doc["projects"].as<JsonArray>();
  for (JsonObject item : projects) {
    if (snapshot.projectRows >= LIST_ROWS) {
      break;
    }
    snapshot.projectName[snapshot.projectRows] = compactProjectName(item, "chat", 10);
    snapshot.projectStatus[snapshot.projectRows] = asciiDisplayText(item["status"] | "running", "running", 24);
    snapshot.projectRows++;
  }

  JsonArray awaiting = doc["awaiting"].as<JsonArray>();
  for (JsonObject item : awaiting) {
    if (snapshot.projectRows >= LIST_ROWS) {
      break;
    }
    snapshot.projectName[snapshot.projectRows] = compactProjectName(item, "resp", 8);
    snapshot.projectStatus[snapshot.projectRows] = "awaiting_response";
    snapshot.projectRows++;
  }
  return snapshot;
}

void drawStatus(JsonDocument &doc) {
  DisplaySnapshot snapshot = buildSnapshot(doc);
  bool force = !uiInitialized;
  if (force) {
    tft.fillScreen(ST77XX_BLACK);
    uiInitialized = true;
  }

  if (force) {
    drawHeaderBase(snapshot);
  } else {
    if (snapshot.wifi != lastSnapshot.wifi) {
      drawWifiStatus(snapshot.wifi);
    }
    if (snapshot.minute != lastSnapshot.minute) {
      drawHeaderTime(snapshot.minute);
    }
  }

  if (force || snapshot.runningProjects != lastSnapshot.runningProjects || snapshot.doneToday != lastSnapshot.doneToday || snapshot.awaitingResponse != lastSnapshot.awaitingResponse) {
    updateMetrics(snapshot, force);
  }
  if (force || !sameProjects(snapshot, lastSnapshot)) {
    updateProjectList(snapshot, force);
  }
  if (staleShown) {
    clearFooter();
  }

  lastSnapshot = snapshot;
  lastRunningProjects = snapshot.runningProjects;
  lastAwaitingResponse = snapshot.awaitingResponse;
  lastDoneToday = snapshot.doneToday;
  animMode = snapshot.mode;
}

void parseAndDraw(const String &line, bool fromWifi = false) {
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    tft.fillScreen(ST77XX_BLACK);
    uiInitialized = false;
    staleShown = false;
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, 40);
    tft.print("JSON ERR");
    tft.setTextSize(1);
    tft.setCursor(8, 72);
    tft.print(error.c_str());
    return;
  }

  hasPacket = true;
  lastPacketAt = millis();
  lastPacketFromWifi = fromWifi;
  drawStatus(doc);
}

#if WIFI_STATUS_ENABLED
void beginWifiIfNeeded() {
  if (wifiConnected()) {
    return;
  }
  if (millis() - lastWifiAttemptAt < WIFI_RETRY_MS) {
    return;
  }
  lastWifiAttemptAt = millis();
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(CODEX_WIFI_SSID, CODEX_WIFI_PASSWORD);
}

void pollWifiStatus() {
  beginWifiIfNeeded();
  if (!wifiConnected()) {
    return;
  }
  if (millis() - lastWifiPollAt < WIFI_POLL_MS) {
    return;
  }
  lastWifiPollAt = millis();

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(CODEX_STATUS_URL)) {
    http.end();
    return;
  }
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    payload.trim();
    if (payload.length() > 0 && payload.length() < MAX_LINE_BYTES) {
      parseAndDraw(payload, true);
    }
  }
  http.end();
}
#endif

void setup() {
  Serial.begin(SERIAL_BAUD);
  inputLine.reserve(MAX_LINE_BYTES);

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setRotation(TFT_ROTATION);
  drawWaiting();

#if WIFI_STATUS_ENABLED
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(CODEX_WIFI_SSID, CODEX_WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
#endif
}

void loop() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      inputLine.trim();
      if (inputLine.length() > 0) {
        parseAndDraw(inputLine);
      }
      inputLine = "";
    } else if (c != '\r') {
      if (inputLine.length() < MAX_LINE_BYTES) {
        inputLine += c;
      } else {
        inputLine = "";
      }
    }
  }

#if WIFI_STATUS_ENABLED
  pollWifiStatus();
#endif

  if (hasPacket && millis() - lastPacketAt > 60000) {
    if (!staleShown) {
      drawFooter("No update > 60s", ST77XX_RED);
      staleShown = true;
    }
  }
  if (hasPacket) {
    updateAnimation();
  }
}
