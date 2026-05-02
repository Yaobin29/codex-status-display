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

// Board/display settings recovered from the existing online_weather sketch.
#define TFT_CS 15
#define TFT_RST 4
#define TFT_DC 2
#define TFT_WIDTH 135
#define TFT_HEIGHT 240
#define SERIAL_BAUD 115200
#define MAX_LINE_BYTES 1400
#define WIFI_POLL_MS 10000
#define WIFI_RETRY_MS 12000

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

String inputLine;
unsigned long lastPacketAt = 0;
bool hasPacket = false;
unsigned long lastAnimAt = 0;
uint8_t animFrame = 0;
uint8_t animMode = 0;
int lastRunningProjects = 0;
int lastAwaitingResponse = 0;
unsigned long lastWifiPollAt = 0;
unsigned long lastWifiAttemptAt = 0;
bool lastPacketFromWifi = false;

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

void drawPixelPulse() {
  const int x0 = 102;
  const int y0 = 20;
  const int size = 5;
  const int gap = 2;
  tft.fillRect(x0 - 1, y0 - 1, 34, 8, ST77XX_BLACK);
  for (int i = 0; i < 5; i++) {
    int phase = (animFrame + i) % 5;
    uint16_t color = ST77XX_BLACK;
    if (phase == 0) {
      color = modeColor(false);
    } else if (phase == 1 || phase == 4) {
      color = modeColor(true);
    }
    tft.fillRect(x0 + i * (size + gap), y0, size, size, color);
  }
}

void updateAnimation() {
  if (millis() - lastAnimAt < 220) {
    return;
  }
  lastAnimAt = millis();
  animFrame = (animFrame + 1) % 5;
  drawRunProgress(16, 121, 103, 7, lastRunningProjects, lastAwaitingResponse);
}

void drawMetricPanel(int x, int y, int w, int h, const char *label, int value, uint16_t color) {
  tft.drawRoundRect(x, y, w, h, 5, color);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(x + 5, y + 6);
  tft.print(label);
  tft.setTextColor(color);
  tft.setTextSize(3);
  tft.setCursor(x + 7, y + 24);
  tft.print(countText(value));
}

uint16_t runningColor(int runningProjects, int awaitingResponse, bool dim) {
  if (runningProjects > 0) {
    return dim ? tft.color565(0, 48, 18) : ST77XX_GREEN;
  }
  return dim ? tft.color565(40, 40, 40) : tft.color565(80, 80, 80);
}

void drawRunProgress(int x, int y, int w, int h, int runningProjects, int awaitingResponse) {
  tft.fillRect(x, y, w, h, ST77XX_BLACK);
  uint16_t color = runningColor(runningProjects, awaitingResponse, false);
  uint16_t dim = runningColor(runningProjects, awaitingResponse, true);
  tft.drawRect(x, y, w, h, dim);
  if (runningProjects <= 0) {
    return;
  }
  const int steps = 13;
  const int cellW = 6;
  const int gap = 2;
  for (int i = 0; i < steps; i++) {
    int phase = (animFrame + i) % steps;
    uint16_t fill = (phase < 4) ? color : dim;
    tft.fillRect(x + 3 + i * (cellW + gap), y + 2, cellW, h - 4, fill);
  }
}

void drawTopStats(int runningProjects, int doneUnseen, int awaitingResponse) {
  lastRunningProjects = runningProjects;
  lastAwaitingResponse = awaitingResponse;
  uint16_t runColor = runningColor(runningProjects, awaitingResponse, false);
  uint16_t doneColor = doneUnseen > 0 ? ST77XX_CYAN : tft.color565(78, 78, 78);

  tft.drawRoundRect(4, 34, 75, 78, 10, runColor);
  tft.drawRoundRect(86, 34, 45, 78, 8, doneColor);

  tft.setTextColor(runColor);
  tft.setTextSize(1);
  tft.setCursor(14, 43);
  tft.print("Running");
  tft.setTextColor(runColor);
  String runText = countText(runningProjects);
  tft.setTextSize(runText.length() > 1 ? 4 : 5);
  tft.setCursor(runText.length() > 1 ? 15 : 21, runText.length() > 1 ? 68 : 63);
  tft.print(runText);

  if (doneUnseen > 0) {
    tft.fillCircle(94, 43, 3, doneColor);
  }
  tft.setTextColor(doneColor);
  tft.setTextSize(1);
  tft.setCursor(101, 43);
  tft.print("Done");
  tft.setTextColor(doneColor);
  String doneText = countText(doneUnseen);
  if (doneText.length() > 2) {
    tft.setTextSize(2);
    tft.setCursor(91, 76);
  } else if (doneText.length() > 1) {
    tft.setTextSize(3);
    tft.setCursor(92, 72);
  } else {
    tft.setTextSize(4);
    tft.setCursor(98, 68);
  }
  tft.print(doneText);
  drawRunProgress(16, 121, 103, 7, runningProjects, awaitingResponse);
}

uint16_t rowColor(const String &status) {
  if (status == "awaiting_response") {
    return tft.color565(218, 74, 16);
  }
  return tft.color565(71, 230, 92);
}

void drawChatRow(int index, const String &name, const String &status) {
  int y = 160 + index * 29;
  uint16_t color = rowColor(status);
  tft.fillRoundRect(9, y + 2, 14, 14, 4, color);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(31, y);
  tft.print(name);
}

void drawRunningList(JsonDocument &doc, int runningProjects) {
  tft.setTextColor(tft.color565(140, 200, 205));
  tft.setTextSize(2);
  tft.setCursor(8, 135);
  tft.print("Running");

  if (runningProjects <= 0) {
    tft.setTextColor(tft.color565(110, 110, 110));
    tft.setTextSize(2);
    tft.setCursor(24, 170);
    tft.print("idle");
    return;
  }

  for (int i = 0; i < 3 && i < runningProjects; i++) {
    String name = compactNameAt(doc, "projects", i, "chat");
    String status = statusAt(doc, "projects", i);
    drawChatRow(i, name, status);
  }
}

void drawWaiting() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 24);
  tft.print("CODEX");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 58);
  tft.print("Waiting for USB JSON");
  tft.setCursor(8, 76);
  tft.print("115200 baud");
}

bool wifiConnected() {
#if WIFI_STATUS_ENABLED
  return WiFi.status() == WL_CONNECTED;
#else
  return false;
#endif
}

void drawWifiIcon(int x, int y) {
  uint16_t color = tft.color565(90, 90, 90);
#if WIFI_STATUS_ENABLED
  color = wifiConnected() ? ST77XX_CYAN : tft.color565(190, 55, 35);
#endif
  tft.drawLine(x + 1, y + 5, x + 8, y, color);
  tft.drawLine(x + 8, y, x + 15, y + 5, color);
  tft.drawLine(x + 4, y + 9, x + 8, y + 6, color);
  tft.drawLine(x + 8, y + 6, x + 12, y + 9, color);
  tft.fillCircle(x + 8, y + 13, 2, color);
}

void drawStatus(JsonDocument &doc) {
  JsonObject counts = doc["counts"];
  int awaitingResponse = counts["awaiting_response"] | 0;
  int runningProjects = counts["running_projects"] | 0;
  int doneUnseen = counts["done_unseen"] | 0;

  tft.fillScreen(ST77XX_BLACK);

  drawWifiIcon(5, 5);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(34, 9);
  tft.print("Codex");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(75, 4);
  tft.print(shortTime(doc["t"] | ""));

  if (awaitingResponse > 0) {
    animMode = 2;
  } else if (doneUnseen > 0) {
    animMode = 1;
  } else {
    animMode = 0;
  }
  drawTopStats(runningProjects, doneUnseen, awaitingResponse);
  drawRunningList(doc, runningProjects);
}

void parseAndDraw(const String &line, bool fromWifi = false) {
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_RED);
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

  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setRotation(0);
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
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.fillRect(4, 224, 127, 12, ST77XX_BLACK);
    tft.setCursor(4, 224);
    tft.print("No update > 60s");
  }
  if (hasPacket) {
    updateAnimation();
  }
}
