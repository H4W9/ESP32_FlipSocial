/* ============================================================================
   ESP32 FlipSocial — Pancake (ESP32-C5, ST7796 320x480, FT6336 capacitive touch)
   ============================================================================
   Standalone touch firmware. Bible-firmware-style UI shell (list menus, header
   with back button + WiFi icon + battery %, footer nav bar) over Picoware's
   panel/touch/HTTP core. Settings + credentials persist to SPIFFS.

   Arduino IDE settings:
     Board            : ESP32C5 Dev Module
     Flash Size       : 8MB
     Partition Scheme : Custom  ->  partitions.csv in this folder
     Flash Frequency  : 80 MHz

   Requires the patched TFT_eSPI-ESP32-C5 library with User_Setup_Select.h set to
   #include <User_Setup_marauder_pancake.h>.
   ============================================================================ */

#include "configs.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "ft6336.h"
#include "TouchKeyboard.h"
#include "theme.h"

// Picoware core (panel init, touch, HTTP). FlipSocial is a native Bible-style app
// in this sketch, so Picoware's own flip_social views are not used.
#include "src/Picoware/internal/boards.hpp"
#include "src/Picoware/internal/gui/draw.hpp"
#include "src/Picoware/internal/system/input.hpp"
#include "src/Picoware/internal/system/http.hpp"
#include "src/Picoware/internal/system/view.hpp"
#include "src/Picoware/internal/system/view_manager.hpp"
using namespace Picoware;

// ── Globals ─────────────────────────────────────────────────────────────────
#ifdef HAS_C5_SD
SPIClass sharedSPI(SPI);
#endif

static ViewManager *vm    = nullptr;   // owns Draw (panel) + InputManager (touch)
static TFT_eSPI    *tft   = nullptr;   // raw panel (from Draw) for the shell screens
static TouchInput  *touch = nullptr;   // FT6336 touch source (from InputManager)
static Theme        theme;             // colour theme + accent + font + brightness

// Theme-driven colours (macros so every use follows the current theme).
#define COL_BG     (theme.bg())
#define COL_FG     (theme.fg())
#define COL_ACCENT (theme.hdr())
#define COL_DIM    (theme.dim())
#define COL_SEL    (theme.sel())
static const uint16_t COL_OK = 0x07E0;   // status green (theme-independent)

static const int SCRW = 320;
static const int SCRH = 480;

// Shell layout — matches ESP32_Bible (header 28, nav 28, list rows 34).
static const int HDRH     = 28;
static const int NAVH     = 28;
static const int ITEMH    = 34;
static const int CONTENTY = HDRH;

// FlipSocial message + viewer result (defined here so Arduino's auto-generated
// prototypes, inserted above the first function, can see these types).
struct FSMsg { uint32_t id; String user, msg, date; int flips, comments; bool flipped; };
struct FSProfile { String bio, joined; int friends; bool ok; };
enum FSVResult { FSV_BACK, FSV_PREV, FSV_NEXT };

// ── Touch helpers ───────────────────────────────────────────────────────────
// Wait for a fresh tap (press edge) and return its point; blocks.
static bool waitTap(uint16_t &x, uint16_t &y, uint32_t timeoutMs = 0) {
  uint32_t start = millis();
  bool wasDown = touch->isPressed();
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) { x = touch->x(); y = touch->y(); return true; }
    wasDown = down;
    if (timeoutMs && (millis() - start) > timeoutMs) return false;
    delay(8);
    yield();
  }
}

static bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return (int)x >= rx && (int)x < rx + rw && (int)y >= ry && (int)y < ry + rh;
}

// ── Theme / brightness plumbing ──────────────────────────────────────────────
static void applyBrightness() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, theme.duty());
#else
  ledcWrite(0, theme.duty());
#endif
}
static void applyThemeToViewManager() {
  if (!vm) return;
  vm->setBackgroundColor(theme.bg());
  vm->setForegroundColor(theme.fg());
  vm->setSelectedColor(theme.sel());
}

// Status RGB LED (onboard addressable LED). Colour-coded by action:
//   yellow = WiFi scan/connect, blue = HTTP fetch, green = success, red = error.
#ifdef RGB_BUILTIN
  #define PW_RGB_PIN RGB_BUILTIN
#else
  #define PW_RGB_PIN LED_BUILTIN
#endif
static void ledRGB(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(PW_RGB_PIN, r, g, b); }
static inline void ledOff()  { ledRGB(0, 0, 0); }
static inline void ledWifi() { ledRGB(45, 30, 0); }   // amber — scanning / connecting
static inline void ledHttp() { ledRGB(0, 0, 55); }    // blue  — HTTP request in flight
static inline void ledOk()   { ledRGB(0, 55, 0); }    // green — success
static inline void ledErr()  { ledRGB(70, 0, 0); }    // red   — error
// Back-compat shim: old on/off calls map to the WiFi (amber) colour.
static void ledSet(bool on) { if (on) ledWifi(); else ledOff(); }

// ── FlipSocial credentials (SPIFFS: /pico_user.json) ─────────────────────────
static String credGet(const char *key) {
  File f = SPIFFS.open("/pico_user.json", FILE_READ);
  if (!f) return "";
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return "";
  return d[key].as<String>();
}
static void credSet(const char *key, const String &val) {
  JsonDocument d;
  File f = SPIFFS.open("/pico_user.json", FILE_READ);
  if (f) { deserializeJson(d, f); f.close(); }
  d[key] = val;
  File w = SPIFFS.open("/pico_user.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}

// ── Saved WiFi networks (SPIFFS: /pico_wifi.json = {"nets":[{"s","p"}]}) ──────
static const int WIFI_MAX_SAVED = 12;
static int wifiLoad(String *ss, String *pp, int maxN) {
  File f = SPIFFS.open("/pico_wifi.json", FILE_READ);
  if (!f) return 0;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return 0;
  JsonArray a = d["nets"].as<JsonArray>();
  if (a.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : a) {
    if (n >= maxN) break;
    ss[n] = v["s"].as<String>();
    pp[n] = v["p"].as<String>();
    n++;
  }
  return n;
}
static void wifiWriteAll(String *ss, String *pp, int n) {
  JsonDocument d;
  JsonArray a = d["nets"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = ss[i];
    o["p"] = pp[i];
  }
  File w = SPIFFS.open("/pico_wifi.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}
// Add/update a network, moving it to the front (most-recent-first).
static void wifiSave(const String &ssid, const String &pass) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  os[m] = ssid; op[m] = pass; m++;                 // new entry first
  for (int i = 0; i < n && m < WIFI_MAX_SAVED; i++) {
    if (ss[i] == ssid) continue;                   // drop old duplicate
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}
static String wifiPassFor(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  for (int i = 0; i < n; i++) if (ss[i] == ssid) return pp[i];
  return "";
}
static void wifiForget(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (ss[i] == ssid) continue;
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}

// ── Battery fuel gauge (MAX17048, I2C 0x36, shared bus) ──────────────────────
// SOC register 0x04: high byte = integer %, low byte = 1/256 % (discarded).
static int      g_battPct = -1;      // -1 = unknown / gauge absent
static bool     g_battOk  = false;
static uint32_t g_battMs  = 0;
static void battInit() {
  Wire.beginTransmission(0x36);
  g_battOk = (Wire.endTransmission() == 0);
  Serial.println(g_battOk ? F("[Battery] MAX17048 OK") : F("[Battery] MAX17048 not found"));
}
static void battUpdate() {
  if (!g_battOk) return;
  Wire.beginTransmission(0x36);
  Wire.write(0x04);                          // SOC register
  if (Wire.endTransmission(false) != 0) { g_battOk = false; return; }
  Wire.requestFrom((uint8_t)0x36, (uint8_t)2);
  if (Wire.available() < 2) return;
  uint8_t hi = Wire.read();
  Wire.read();                               // fractional byte — discard
  g_battPct = (hi > 100) ? 100 : hi;
  g_battMs  = millis();
}

// True while a saved-network connect attempt is in flight (header icon = yellow).
static volatile bool g_wifiConnecting = false;

// ── Rendering helpers ─────────────────────────────────────────────────────────
// One 90°-wide WiFi arc (a real wifi-fan wedge: ±45° around straight up),
// plotted point-by-point so it doesn't depend on any drawArc angle convention.
static void wifiArc(int cx, int cy, int r, uint16_t c) {
  for (int deg = -45; deg <= 45; deg += 2) {
    float a = deg * 0.0174533f;
    int x = cx + (int)lroundf(r * sinf(a));
    int y = cy - (int)lroundf(r * cosf(a));
    tft->drawPixel(x, y, c);              // 1px-thin arc
  }
}

// Battery % (right edge) + WiFi state icon, painted into the header's top-right.
// Self-clearing, so it can also be called on its own for a periodic refresh.
static void drawHeaderStatus() {
  if (g_battOk && (g_battMs == 0 || millis() - g_battMs > 10000)) battUpdate();
  tft->fillRect(SCRW - 62, 0, 62, HDRH, COL_ACCENT);   // clear the status corner

  int rx = SCRW - 4;                                   // right edge for battery text
  if (g_battPct >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_battPct);
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MR_DATUM);
    tft->drawString(pct, rx, HDRH / 2, 1);             // small (font 1) like ESP32_Bible
    rx -= tft->textWidth(pct, 1) + 8;                  // slot the icon left of the %
  }

  // WiFi icon: source dot + three 90° arcs. green=connected, yellow=connecting, red=off.
  uint16_t wc = g_wifiConnecting               ? TFT_YELLOW
              : (WiFi.status() == WL_CONNECTED) ? COL_OK
                                                : TFT_RED;
  int cx = rx - 10, cy = HDRH / 2 + 5;                 // arc apex (bottom) point
  tft->fillCircle(cx, cy, 1, wc);
  wifiArc(cx, cy, 4,  wc);
  wifiArc(cx, cy, 7,  wc);
  wifiArc(cx, cy, 10, wc);
  tft->setTextDatum(TL_DATUM);
}

// Crisp vector chevron "<"/">" (solid triangle) — matches ESP32_Bible selectors.
static void drawChevron(int bx, int by, int bw, int bh, bool right, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2;
  if (right) tft->fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, col);
  else       tft->fillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, col);
}
// Centered "+"/"-" (2px strokes) for the brightness selector.
static void drawPlusMinus(int bx, int by, int bw, int bh, bool plus, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
  tft->fillRect(cx - r, cy - 1, 2 * r, 2, col);          // horizontal
  if (plus) tft->fillRect(cx - 1, cy - r, 2, 2 * r, col); // vertical
}

// ESP32_Bible-style header: optional back box with chevron (top-left), centred
// title, status corner (WiFi icon + battery %) top-right.
static void drawHeader(const String &title, bool showBack) {
  tft->fillRect(0, 0, SCRW, HDRH, COL_ACCENT);
  if (showBack) {
    tft->fillRoundRect(2, 3, 40, 22, 4, COL_ACCENT);
    tft->drawRoundRect(2, 3, 40, 22, 4, theme.neon(3, COL_DIM));
    drawChevron(2, 3, 40, 22, false, COL_FG);
  }
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  tft->drawString(title, SCRW / 2, HDRH / 2, 2);
  drawHeaderStatus();
  tft->setTextDatum(TL_DATUM);
}
static bool backTapped(uint16_t x, uint16_t y) {
  return (int)y < HDRH && (int)x < 48;   // top-left back box
}

// Footer nav bar (Bible-style): up to three labelled rounded buttons in thirds.
static void drawNav(const char *l, const char *m, const char *r) {
  int y = SCRH - NAVH, third = SCRW / 3, bh = NAVH - 10, by = y + 5, bw = third - 10;
  tft->fillRect(0, y, SCRW, NAVH, COL_BG);
  tft->drawFastHLine(0, y, SCRW, theme.edge());
  const char *L[3] = { l, m, r };
  for (int i = 0; i < 3; i++) {
    if (!L[i] || !L[i][0]) continue;
    int cx = i * third + third / 2, bx = cx - bw / 2;
    tft->fillRoundRect(bx, by, bw, bh, 5, COL_ACCENT);
    tft->drawRoundRect(bx, by, bw, bh, 5, theme.neon(i, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(L[i], cx, by + bh / 2, 2);
  }
  tft->setTextDatum(TL_DATUM);
}
// Which nav third was tapped: 0/1/2, or -1 if not in the footer band.
static int navHit(uint16_t x, uint16_t y) {
  if ((int)y < SCRH - NAVH) return -1;
  int c = (int)x / (SCRW / 3);
  return c > 2 ? 2 : c;
}

// One list row (ESP32_Bible-style): fill, left text, optional right chevron, divider.
static void drawListRow(int y, const String &text, bool sel, bool arrow) {
  uint16_t bgc = sel ? COL_SEL : COL_BG;
  tft->fillRect(0, y, SCRW, ITEMH, bgc);
  tft->setTextColor(COL_FG, bgc);
  tft->setTextDatum(ML_DATUM);
  tft->drawString(text, 12, y + ITEMH / 2, 2);
  if (arrow) drawChevron(SCRW - 26, y, 16, ITEMH, true, COL_DIM);
  tft->drawFastHLine(0, y + ITEMH - 1, SCRW, theme.edge());
  tft->setTextDatum(TL_DATUM);
}

// Sprite version of a list row (for flicker-free momentum scrolling).
static void drawRowSprite(TFT_eSprite &spr, int y, const String &text, bool arrow) {
  spr.fillRect(0, y, SCRW, ITEMH, COL_BG);
  spr.setTextColor(COL_FG, COL_BG);
  spr.setTextDatum(ML_DATUM);
  spr.drawString(text, 12, y + ITEMH / 2, 2);
  if (arrow) {
    int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
    spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, COL_DIM);
  }
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.edge());
  spr.setTextDatum(TL_DATUM);
}

// Momentum-scrolling list of string rows (no paging). Returns the tapped row
// index, or -1 if the Back button was tapped.
static int scrollList(const String &title, String *rows, int n, bool arrow) {
  const int CY = CONTENTY, CH = SCRH - CONTENTY;
  int total = n * ITEMH;
  tft->fillScreen(COL_BG);
  drawHeader(title, true);

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (haveSpr) {
      spr.fillSprite(COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawRowSprite(spr, y, rows[i], arrow);
      }
      spr.pushSprite(0, CY);
    } else {
      tft->fillRect(0, CY, SCRW, CH, COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawListRow(CY + y, rows[i], false, arrow);
      }
    }
  };
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); return -1; }
        if ((int)pY >= CY) {
          int idx = ((int)pY - CY + (int)scroll) / ITEMH;
          if (idx >= 0 && idx < n) { if (haveSpr) spr.deleteSprite(); return idx; }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }

    wasDown = down;
    if (need) render();
    delay(12);
  }
}

// Bottom status line (only on screens WITHOUT a footer nav bar).
static void statusLine(const char *msg, uint16_t col = 0xFFFF) {
  tft->fillRect(0, SCRH - 26, SCRW, 26, COL_BG);
  tft->setTextColor(col == 0xFFFF ? COL_FG : col, COL_BG);
  tft->setTextDatum(ML_DATUM);
  tft->drawString(msg, 8, SCRH - 13, 2);
  tft->setTextDatum(TL_DATUM);
}

// ── Settings chip rows: label + [<] value [>] (or [-] value [+]) ─────────────
// ── Settings rows (ESP32_Bible choiceRow layout) ─────────────────────────────
// [<] value [>] with fixed-right buttons (28x22). Selected row highlights with
// sel_bg. `valcol` = colour to draw the value text (0 = follow font colour).
static const int CHIP_W = 28, CHIP_H = 22;
// Geometry helper (draw + hit-test share it): fwd/bwd button x for a row value.
static void chipGeom(const String &val, int &fwd_bx, int &bwd_bx, int &vx) {
  fwd_bx = SCRW - 8 - CHIP_W;
  int vw = tft->textWidth(val.c_str(), 2);
  vx     = fwd_bx - 4 - vw;
  bwd_bx = vx - 4 - CHIP_W;
}
static void drawChipRow(int y, const String &label, const String &val, bool pm,
                        bool sel, uint16_t valcol) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  drawListRow(y, label, sel, false);
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  int seed = y / ITEMH;
  tft->fillRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  tft->drawRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed, COL_DIM));
  tft->fillRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  tft->drawRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed + 4, COL_DIM));
  if (pm) {
    drawPlusMinus(bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    drawPlusMinus(fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  } else {
    drawChevron(bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    drawChevron(fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  }
  tft->setTextColor(valcol ? valcol : COL_FG, rbg);
  tft->setTextDatum(ML_DATUM);
  tft->drawString(val, vx, y + ITEMH / 2, 2);
  tft->setTextDatum(TL_DATUM);
}
// Returns -1 none, 0 left/decrement, 1 right/increment. `val` must match draw.
static int chipHit(int y, const String &val, uint16_t x, uint16_t ty) {
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  if ((int)ty < by || (int)ty >= by + CHIP_H) return -1;
  if ((int)x >= fwd_bx && (int)x < fwd_bx + CHIP_W) return 1;
  if ((int)x >= bwd_bx && (int)x < bwd_bx + CHIP_W) return 0;
  return -1;
}
// Label + right-aligned dim value + arrow (WiFi / creds rows).
static void drawInfoRow(int y, const String &label, const String &val, bool sel) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  drawListRow(y, label, sel, true);
  if (val.length()) {
    tft->setTextColor(COL_DIM, rbg);
    tft->setTextDatum(MR_DATUM);
    tft->drawString(val, SCRW - 26, y + ITEMH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
}

// Simple centred two-line message screen with a Back header (blocks for a tap).
static void msgScreen(const char *title, const String &a, const String &b, uint16_t col) {
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  tft->setTextColor(col, COL_BG);
  tft->setTextDatum(MC_DATUM);
  tft->drawString(a, SCRW / 2, SCRH / 2 - 12, 2);
  if (b.length()) {
    tft->setTextColor(COL_DIM, COL_BG);
    tft->drawString(b, SCRW / 2, SCRH / 2 + 14, 2);
  }
  tft->setTextDatum(TL_DATUM);
  uint16_t x, y; waitTap(x, y);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// Last STA disconnect reason (WIFI_REASON_*): 15 = 4-way handshake timeout
// (usually wrong password), 201 = no AP found (band/channel), 205 = conn fail.
static volatile int g_wifiReason = 0;
static volatile int g_wifiEvt = -1;   // last Arduino WiFi event id (-1 = none seen)
static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  g_wifiEvt = (int)event;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    g_wifiReason = info.wifi_sta_disconnected.reason;
}

// Wait up to timeoutMs for association, updating the on-screen status/reason.
// Tapping the screen aborts the wait (returns false) so the user is never stuck.
static bool waitConnect(uint32_t timeoutMs) {
  uint32_t start = millis();
  int last = -999;
  bool wasDown = touch->isPressed();
  statusLine("Connecting...  tap to cancel");
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) return false;    // user cancelled
    wasDown = down;
    int s = WiFi.status();
    if (s != last) {
      last = s;
      tft->fillRect(0, 78, SCRW, 20, COL_BG);
      tft->setTextColor(COL_FG, COL_BG);
      tft->drawString(String("st ") + s + "  ev " + g_wifiEvt + "  rsn " + g_wifiReason, 10, 78, 2);
    }
    delay(60);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Blocking connect with on-screen progress. Tries the exact scanned AP first
// (channel/BSSID — band-safe on the dual-band C5), then falls back to a plain
// SSID+password attempt. Returns true on success.
static bool connectWiFi(const String &ssid, const String &pass,
                        int32_t channel = 0, const uint8_t *bssid = nullptr) {
  g_wifiConnecting = true;
  ledSet(true);
  tft->fillScreen(COL_BG);
  drawHeader("WiFi", false);
  bool band5 = channel > 14;
  statusLine((String(ssid) + "   ch " + channel + (channel ? (band5 ? " (5GHz)" : " (2.4GHz)") : "")).c_str());

  g_wifiReason = 0;
  g_wifiEvt = -1;              // reset so we can see fresh STA events this attempt
  WiFi.persistent(false);
  WiFi.setSleep(false);
  // After WiFi.scanNetworks() the STA is left wedged — begin() produces no
  // STA_START (last event stays SCAN_DONE/102). A gentle disconnect() doesn't
  // clear it; fully cycle the radio OFF then back to STA for a clean start.
  WiFi.scanDelete();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.mode(WIFI_STA);
  delay(200);

  bool ok = false;
  if (channel > 0 && bssid) {                        // Attempt 1: pinned to AP
    tft->setTextColor(COL_FG, COL_BG);
    tft->drawString("try 1: pinned AP", 10, 56, 2);
    WiFi.begin(ssid.c_str(), pass.c_str(), channel, bssid);
    ok = waitConnect(12000);
  }
  if (!ok) {                                         // Attempt 2: driver chooses
    tft->fillRect(0, 56, SCRW, 20, COL_BG);
    tft->setTextColor(COL_FG, COL_BG);
    tft->drawString("try 2: auto", 10, 56, 2);
    g_wifiReason = 0;
    WiFi.disconnect();
    delay(150);
    WiFi.begin(ssid.c_str(), pass.c_str());
    ok = waitConnect(12000);
  }
  g_wifiConnecting = false;
  ledSet(false);
  return ok;
}

// Scan, then connect to a saved SSID pinned to its live channel/BSSID.
static bool connectSaved(const String &ssid) {
  String pass = wifiPassFor(ssid);
  g_wifiConnecting = true;
  ledSet(true);
  tft->fillScreen(COL_BG);
  drawHeader("WiFi", true);
  statusLine((String("Scanning for ") + ssid + " ...").c_str());
  int cnt = WiFi.scanNetworks();
  int found = -1;
  for (int j = 0; j < cnt; j++) if (WiFi.SSID(j) == ssid) { found = j; break; }
  bool ok;
  if (found >= 0) {
    uint8_t b[6]; const uint8_t *bp = WiFi.BSSID(found); if (bp) memcpy(b, bp, 6);
    ok = connectWiFi(ssid, pass, WiFi.channel(found), bp ? b : nullptr);
  } else {
    ok = connectWiFi(ssid, pass);
  }
  g_wifiConnecting = false;
  ledSet(false);
  return ok;
}

// Boot auto-connect — NON-BLOCKING state machine so the main menu shows instantly
// and WiFi associates in the background. Progress is shown by the header WiFi icon
// (yellow=connecting) and the activity LED. Driven from loop() via wifiBgTick().
enum WbState { WB_IDLE, WB_SCAN_POLL, WB_CYCLE_STA, WB_BEGIN, WB_CONNECT, WB_DONE };
static WbState  g_wb  = WB_IDLE;
static uint32_t g_wbT = 0;
static String   g_wbSsid, g_wbPass;   // chosen network, carried across states
static int32_t  g_wbCh = 0;
static uint8_t  g_wbBssid[6];
static bool     g_wbHave = false;     // have a pinned channel/BSSID

static void wifiBgBegin() {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  if (wifiLoad(ss, pp, WIFI_MAX_SAVED) == 0) { g_wb = WB_IDLE; return; }
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.scanNetworks(true);           // async scan (STA is already up from setup)
  g_wbT = millis();
  g_wb  = WB_SCAN_POLL;
  g_wifiConnecting = true;
}

static void wifiBgTick() {
  switch (g_wb) {
    case WB_IDLE:
    case WB_DONE:
      return;
    case WB_SCAN_POLL: {
      int r = WiFi.scanComplete();
      if (r == WIFI_SCAN_RUNNING) {              // still scanning
        if (millis() - g_wbT > 8000) { WiFi.scanDelete(); g_wifiConnecting = false; g_wb = WB_DONE; }
        return;
      }
      String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
      int nsv = wifiLoad(ss, pp, WIFI_MAX_SAVED);
      int pick = -1, netIdx = -1;
      if (r > 0)
        for (int i = 0; i < nsv && pick < 0; i++)
          for (int j = 0; j < r; j++)
            if (WiFi.SSID(j) == ss[i]) { pick = j; netIdx = i; break; }
      if (pick >= 0) {
        g_wbSsid = ss[netIdx]; g_wbPass = pp[netIdx];
        g_wbCh   = WiFi.channel(pick);
        const uint8_t *bp = WiFi.BSSID(pick);
        g_wbHave = (bp != nullptr);
        if (bp) memcpy(g_wbBssid, bp, 6);
      } else {
        g_wbSsid = ss[0]; g_wbPass = pp[0]; g_wbHave = false;   // best effort
      }
      WiFi.scanDelete();
      // Cycle the radio AFTER the scan, BEFORE begin() — clears the post-scan
      // STA wedge on the C5 (settle time handled by the timed states below).
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      g_wbT = millis(); g_wb = WB_CYCLE_STA;
      return;
    }
    case WB_CYCLE_STA:
      if (millis() - g_wbT < 300) return;
      WiFi.mode(WIFI_STA);
      g_wbT = millis(); g_wb = WB_BEGIN;
      return;
    case WB_BEGIN:
      if (millis() - g_wbT < 200) return;
      g_wifiReason = 0; g_wifiEvt = -1;
      if (g_wbHave) WiFi.begin(g_wbSsid.c_str(), g_wbPass.c_str(), g_wbCh, g_wbBssid);
      else          WiFi.begin(g_wbSsid.c_str(), g_wbPass.c_str());
      g_wbT = millis(); g_wb = WB_CONNECT;
      return;
    case WB_CONNECT:
      if (WiFi.status() == WL_CONNECTED) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
      if (millis() - g_wbT > 15000)      { g_wifiConnecting = false; g_wb = WB_DONE; return; }
      return;
  }
}

// Test HTTPS GET through Picoware's HTTP class; render the first lines.
static void httpTest() {
  tft->fillScreen(COL_BG);
  drawHeader("HTTP Test", true);
  statusLine("GET https://httpbin.org/get ...");
  HTTP http;
  String resp = http.request("GET", "https://httpbin.org/get");
  tft->setTextColor(COL_FG, COL_BG);
  int y = 50, start = 0;
  for (int i = 0; i < (int)resp.length() && y < SCRH - 40; i++) {
    if (resp[i] == '\n' || i - start > 44) {
      tft->drawString(resp.substring(start, i), 6, y, 1);
      y += 12;
      start = i + 1;
    }
  }
  if (resp.length() == 0) statusLine("No response (check TLS / connection).", TFT_RED);
  else                    statusLine("Tap to continue.", COL_OK);
  uint16_t x, ty; waitTap(x, ty);
}

// Scan / pick / password / connect flow. Smooth-scroll list, no paging.
// Row 0 = "Rescan"; the rest are scanned SSIDs.
static void scanFlow() {
  static String rows[41];
  for (;;) {
    tft->fillScreen(COL_BG);
    drawHeader("Scan", true);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("Scanning...", SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
    ledSet(true);
    int nnet = WiFi.scanNetworks();
    ledSet(false);
    int rc = (nnet < 0) ? 0 : nnet;

    int n = 0;
    rows[n++] = "Rescan";
    for (int i = 0; i < rc && n < 41; i++)
      rows[n++] = WiFi.SSID(i) + "  (" + WiFi.RSSI(i) + ")";

    int sel = scrollList("Scan", rows, n, true);
    if (sel < 0) return;                               // Back
    if (sel == 0) continue;                            // Rescan

    int idx = sel - 1;
    if (idx < 0 || idx >= rc) continue;
    String ssid = WiFi.SSID(idx);
    int32_t ch = WiFi.channel(idx);
    uint8_t b[6]; const uint8_t *bp = WiFi.BSSID(idx); if (bp) memcpy(b, bp, 6);
    char pass[65] = {0};
    String sp = wifiPassFor(ssid);
    if (sp.length()) strncpy(pass, sp.c_str(), sizeof(pass) - 1);
    if (!touchKeyboardInput(*tft, COL_FG, COL_BG, pass, sizeof(pass),
                            (String("Password: ") + ssid).c_str(), true)) continue;
    if (connectWiFi(ssid, pass, ch, bp ? b : nullptr)) {
      wifiSave(ssid, pass);
      statusLine("Connected!", COL_OK);
      uint16_t a, bb; waitTap(a, bb);
      return;
    }
    statusLine((String("Failed (reason ") + g_wifiReason + "). Tap to re-scan.").c_str(), TFT_RED);
    uint16_t a, bb; waitTap(a, bb);
  }
}

// WiFi Setup: smooth-scroll list of saved networks (tap to connect), a Scan row,
// and a Forget row. No paging.
static void wifiSetup() {
  static String rows[WIFI_MAX_SAVED + 2];
  for (;;) {
    String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
    int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
    int r = 0;
    for (int i = 0; i < n; i++) {
      bool cur = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ss[i]);
      rows[r++] = (cur ? String("* ") : String("")) + ss[i];
    }
    int scanRow   = r; rows[r++] = "+ Scan for networks";
    int forgetRow = (n > 0) ? r : -1;
    if (n > 0) rows[r++] = "- Forget a network";

    int sel = scrollList("WiFi Setup", rows, r, true);
    if (sel < 0) return;
    if (sel < n)              connectSaved(ss[sel]);
    else if (sel == scanRow)  scanFlow();
    else if (sel == forgetRow) {                       // pick a saved net to forget
      static String frows[WIFI_MAX_SAVED];
      for (int i = 0; i < n; i++) frows[i] = ss[i];
      int f = scrollList("Forget", frows, n, true);
      if (f >= 0 && f < n) wifiForget(ss[f]);
    }
  }
}

// WiFi Debug: live status/event/reason + heap, with HTTP-test/reconnect actions.
static void wifiDebug() {
  for (;;) {
    tft->fillScreen(COL_BG);
    drawHeader("WiFi Debug", true);
    int y = CONTENTY + 10;
    tft->setTextColor(COL_FG, COL_BG);
    tft->setTextDatum(TL_DATUM);
    auto line = [&](const String &s) { tft->drawString(s, 12, y, 2); y += 24; };
    bool up = (WiFi.status() == WL_CONNECTED);
    line(String("Status:       ") + WiFi.status() + (up ? "  (connected)" : ""));
    line(String("Last event:   ") + g_wifiEvt);
    line(String("Disc reason:  ") + g_wifiReason);
    line(String("SSID:         ") + (up ? WiFi.SSID() : String("-")));
    line(String("IP:           ") + (up ? WiFi.localIP().toString() : String("-")));
    line(String("RSSI:         ") + (up ? String(WiFi.RSSI()) : String("-")));
    line(String("Free heap:    ") + ESP.getFreeHeap());
    line(String("Free PSRAM:   ") + ESP.getFreePsram());
    drawNav("Back", "HTTP Test", "Reconnect");

    uint16_t x, ty;
    if (!waitTap(x, ty)) continue;
    if (backTapped(x, ty)) return;
    int nh = navHit(x, ty);
    if (nh == 0) return;
    if (nh == 1) httpTest();
    if (nh == 2) {
      String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
      int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
      if (n) connectSaved(ss[0]);
    }
  }
}

// ── Settings (ESP32_Bible layout: highlight on tap, partial redraw, no flash) ──
static const int SET_N = 8;   // Theme, Accent, Font Color, Brightness, WiFi, Debug, User, Pass
// Value string for the two chip rows that need it for hit-testing.
static String setChipVal(int row) {
  switch (row) {
    case 0: return theme.themeName();
    case 1: return theme.accentName();
    case 2: return theme.fontColName();
    case 3: return String(theme.bright + 1) + "/20";
  }
  return "";
}
// Draw one settings row at its slot, highlighted if `sel`. No fillScreen.
static void drawSettingRow(int row, int sel) {
  int y = CONTENTY + row * ITEMH;
  bool s = (row == sel);
  switch (row) {
    case 0: drawChipRow(y, "Theme",      theme.themeName(),  false, s, 0); break;
    case 1: drawChipRow(y, "Accent",     theme.accentName(), false, s, 0); break;
    case 2: drawChipRow(y, "Font Color", theme.fontColName(), false, s, theme.fontColPreview()); break;
    case 3: drawChipRow(y, "Brightness", setChipVal(3), true, s, 0); break;
    case 4: drawInfoRow(y, "WiFi Setup", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""), s); break;
    case 5: drawInfoRow(y, "WiFi Debug", "", s); break;
    case 6: drawInfoRow(y, "Username",   credGet("user"), s); break;
    case 7: drawInfoRow(y, "Password",   credGet("pass").length() ? String("****") : String(""), s); break;
  }
}
static void settingsFlow() {
  int sel = -1;
  auto full = [&]() {                         // full repaint (theme/bg changed)
    tft->fillScreen(COL_BG);
    drawHeader("Settings", true);
    for (int i = 0; i < SET_N; i++) drawSettingRow(i, sel);
  };
  auto recolor = [&]() {                      // font colour changed — no fillScreen
    drawHeader("Settings", true);
    for (int i = 0; i < SET_N; i++) drawSettingRow(i, sel);
  };
  full();

  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    if (backTapped(x, y)) return;
    if ((int)y < CONTENTY) continue;
    int row = ((int)y - CONTENTY) / ITEMH;
    if (row < 0 || row >= SET_N) continue;

    // Move the highlight to the tapped row (partial redraw of old + new).
    int old = sel; sel = row;
    if (old != row) { if (old >= 0) drawSettingRow(old, sel); drawSettingRow(row, sel); }

    int h = (row <= 3) ? chipHit(CONTENTY + row * ITEMH, setChipVal(row), x, y) : -1;
    switch (row) {
      case 0: if (h >= 0) { theme.cycleTheme(h); theme.save(); applyThemeToViewManager(); full(); } break;
      case 1: if (h >= 0) { theme.cycleAccent(h); theme.save(); applyThemeToViewManager(); drawSettingRow(1, sel); } break;
      case 2: if (h >= 0) { theme.cycleFontCol(h); theme.save(); applyThemeToViewManager(); recolor(); } break;
      case 3: if (h == 0 && theme.bright > 0)  theme.bright--;
              else if (h == 1 && theme.bright < 19) theme.bright++;
              if (h >= 0) { theme.save(); applyBrightness(); drawSettingRow(3, sel); } break;
      case 4: wifiSetup(); full(); break;
      case 5: wifiDebug(); full(); break;
      case 6: { char b[64] = {0}; String u = credGet("user"); strncpy(b, u.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "FlipSocial User:", false))
                  credSet("user", String(b));
                full(); } break;
      case 7: { char b[64] = {0}; String p = credGet("pass"); strncpy(b, p.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "FlipSocial Password:", true))
                  credSet("pass", String(b));
                full(); } break;
      default: break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  FlipSocial — native Bible-style client (chat bubbles, smooth scroll, flips,
//  comments, feed paging). Uses the jblanked.com feed API.
// ═════════════════════════════════════════════════════════════════════════════
#define FS_MAX 40
static const int FS_LH = 18;          // line height (font 2)
static const int FS_BM = 8;           // bubble margin
static const int FS_BP = 8;           // bubble padding
enum { FS_FEED = 0, FS_COMMENTS = 1, FS_MYPOSTS = 2, FS_MESSAGES = 3 };
static String g_msgPeer;   // current message-thread peer (for reload/send)

static String fsUser() { return credGet("user"); }

static String fsRequest(const char *method, const String &url, const String &payload = "") {
  HTTP http;
  String u = credGet("user");
  String p = credGet("pass");
  const char *hk[] = {"Content-Type", "HTTP_USER_AGENT", "HTTP_ACCEPT", "username", "password"};
  const char *hv[] = {"application/json", "Pico", "X-Flipper-Redirect", u.c_str(), p.c_str()};
  if (vm) vm->getLED().on();                 // activity LED on during the fetch (as Picoware did)
  String r = http.request(method, url, payload, hk, hv, 5);
  if (vm) vm->getLED().off();
  return r;
}

static bool fsBool(JsonVariant v) {
  if (v.is<bool>()) return v.as<bool>();
  String s = v.as<String>();
  return s == "true" || s == "True" || s == "1";
}

static int fsLoadFeed(FSMsg *arr, int series) {
  String url = "https://www.jblanked.com/flipper/api/feed/20/" + fsUser() + "/" + String(series) + "/max/series/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray f = doc["feed"].as<JsonArray>();
  if (f.isNull()) return 0;
  int n = 0;
  for (JsonVariant it : f) {
    if (n >= FS_MAX) break;
    FSMsg &m = arr[n++];
    m.id = it["id"].as<uint32_t>();
    m.user = it["username"].as<String>();
    m.msg = it["message"].as<String>();
    m.date = it["date_created"].as<String>();
    m.flips = it["flip_count"].as<int>();
    m.comments = it["comment_count"].as<int>();
    m.flipped = fsBool(it["flipped"]);
  }
  return n;
}

static int fsLoadComments(FSMsg *arr, uint32_t postId) {
  String url = "https://www.jblanked.com/flipper/api/feed/comments/20/" + fsUser() + "/" + String(postId) + "/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray f = doc["comments"].as<JsonArray>();
  if (f.isNull()) return 0;
  int n = 0;
  for (JsonVariant it : f) {
    if (n >= FS_MAX) break;
    FSMsg &m = arr[n++];
    m.id = it["id"].as<uint32_t>();
    m.user = it["username"].as<String>();
    m.msg = it["message"].as<String>();
    m.date = it["date_created"].as<String>();
    m.flips = it["flip_count"].as<int>();
    m.comments = 0;
    m.flipped = fsBool(it["flipped"]);
  }
  return n;
}

static void fsFlip(FSMsg &m) {
  String payload = String("{\"username\":\"") + fsUser() + "\",\"post_id\":\"" + m.id + "\"}";
  fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/flip/", payload);
  m.flipped = !m.flipped;
  m.flips += m.flipped ? 1 : -1;
  if (m.flips < 0) m.flips = 0;
}

// ── Profile / friends API (jblanked user endpoints) ───────────────────────────
// GET /user/profile/{user}/ -> {"bio","friends_count","date_created"}
static FSProfile fsLoadProfile(const String &who) {
  FSProfile p; p.ok = false; p.friends = 0;
  String resp = fsRequest("GET", "https://www.jblanked.com/flipper/api/user/profile/" + who + "/");
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return p;
  if (doc["bio"].isNull() && doc["friends_count"].isNull()) return p;
  p.bio     = doc["bio"].as<String>();
  p.friends = doc["friends_count"].as<int>();
  p.joined  = doc["date_created"].as<String>();
  p.ok      = true;
  return p;
}
static bool fsChangeBio(const String &bio) {
  String payload = String("{\"username\":\"") + fsUser() + "\",\"bio\":\"" + bio + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/change-bio/", payload);
  return r.indexOf("SUCCESS") != -1 || r.indexOf("success") != -1;
}
// GET /user/friends/{user}/{max}/ -> {"friends":[username, ...]}
static int fsLoadFriends(String *arr, int maxN) {
  String url = "https://www.jblanked.com/flipper/api/user/friends/" + fsUser() + "/" + String(maxN) + "/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray f = doc["friends"].as<JsonArray>();
  if (f.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : f) { if (n >= maxN) break; arr[n++] = v.as<String>(); }
  return n;
}
static bool fsAddFriend(const String &friendName) {
  String payload = String("{\"username\":\"") + fsUser() + "\",\"friend\":\"" + friendName + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/add-friend/", payload);
  return r.indexOf("SUCCESS") != -1 || r.indexOf("success") != -1;
}
static bool fsRemoveFriend(const String &friendName) {
  String payload = String("{\"username\":\"") + fsUser() + "\",\"friend\":\"" + friendName + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/remove-friend/", payload);
  return r.indexOf("SUCCESS") != -1 || r.indexOf("success") != -1;
}

// ── Messages API (jblanked messages endpoints) ────────────────────────────────
// GET /messages/{me}/get/list/{max}/ -> {"users":[username, ...]} (conversations)
static int fsLoadMsgUsers(String *arr, int maxN) {
  String url = "https://www.jblanked.com/flipper/api/messages/" + fsUser() + "/get/list/" + String(maxN) + "/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray u = doc["users"].as<JsonArray>();
  if (u.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : u) { if (n >= maxN) break; arr[n++] = v.as<String>(); }
  return n;
}
// GET /messages/{me}/get/{peer}/{max}/ -> {"conversations":[{"sender","content","date_created"}]}
static int fsLoadMessages(FSMsg *arr, const String &peer) {
  String url = "https://www.jblanked.com/flipper/api/messages/" + fsUser() + "/get/" + peer + "/40/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray c = doc["conversations"].as<JsonArray>();
  if (c.isNull()) return 0;
  int n = 0;
  for (JsonVariant it : c) {
    if (n >= FS_MAX) break;
    FSMsg &m = arr[n++];
    m.user = it["sender"].as<String>();
    m.msg  = it["content"].as<String>();
    m.date = it["date_created"].as<String>();
    m.id = 0; m.flips = 0; m.comments = 0; m.flipped = false;
  }
  return n;
}
// POST /messages/{me}/post/  body {"receiver","content"}
static bool fsSendMessage(const String &peer, const String &content) {
  String payload = String("{\"receiver\":\"") + peer + "\",\"content\":\"" + content + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/messages/" + fsUser() + "/post/", payload);
  return r.indexOf("ERROR") == -1 && r.length() > 0;
}

// GET /user/explore/{keyword}/{max}/ -> {"users":[username, ...]}
static int fsExplore(const String &keyword, String *arr, int maxN) {
  String url = "https://www.jblanked.com/flipper/api/user/explore/" + keyword + "/" + String(maxN) + "/";
  String resp = fsRequest("GET", url);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray u = doc["users"].as<JsonArray>();
  if (u.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : u) { if (n >= maxN) break; arr[n++] = v.as<String>(); }
  return n;
}

static void fsAddComment(uint32_t postId) {
  char b[256] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Comment:", false)) return;
  if (strlen(b) == 0) return;
  String payload = String("{\"username\":\"") + fsUser() + "\",\"content\":\"" + b + "\",\"post_id\":\"" + postId + "\"}";
  fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/comment/", payload);
}

static void fsPost() {
  char b[256] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "New Post:", false)) return;
  if (strlen(b) == 0) return;
  tft->fillScreen(COL_BG); drawHeader("New Post", true); statusLine("Posting...");
  String payload = String("{\"username\":\"") + fsUser() + "\",\"content\":\"" + b + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/post/", payload);
  bool bad = (r.indexOf("ERROR") != -1 || r.length() == 0);
  statusLine(bad ? "Post failed. Tap." : "Posted! Tap.", bad ? TFT_RED : COL_OK);
  uint16_t x, y; waitTap(x, y);
}

// Word-wrap `s` to `maxW` pixels (font 2). If `out` is non-null, fills lines;
// returns the line count.
static int fsWrap(const String &s, int maxW, String *out, int maxLines) {
  int count = 0;
  String line = "";
  auto flush = [&]() { if (out && count < maxLines) out[count] = line; count++; line = ""; };
  int i = 0, n = s.length();
  while (i < n) {
    if (s[i] == '\n') { flush(); i++; continue; }
    int j = i;
    while (j < n && s[j] != ' ' && s[j] != '\n') j++;
    String word = s.substring(i, j);
    String cand = line.length() ? line + " " + word : word;
    if (tft->textWidth(cand.c_str(), 2) <= maxW) {
      line = cand;
    } else {
      if (line.length()) flush();
      line = word;
      while (tft->textWidth(line.c_str(), 2) > maxW && line.length() > 1) {
        int k = line.length();
        while (k > 1 && tft->textWidth(line.substring(0, k).c_str(), 2) > maxW) k--;
        String part = line.substring(0, k);
        line = line.substring(k);
        String keep = line; line = part; flush(); line = keep;
      }
    }
    i = j;
    while (i < n && s[i] == ' ') i++;
  }
  if (line.length() || count == 0) flush();
  return count;
}

static int fsBubbleH(const FSMsg &m, int mode) {
  int nl = fsWrap(m.msg, SCRW - 2 * FS_BM - 2 * FS_BP, nullptr, 0);
  if (nl < 1) nl = 1;
  // Messages: sender line + body only. Feed/comments: + a flips/counts footer line.
  if (mode == FS_MESSAGES) return FS_BP + FS_LH + nl * FS_LH + FS_BP;
  return FS_BP + FS_LH + nl * FS_LH + 6 + FS_LH + FS_BP;
}

// Draw one bubble at content-space top `yTop` into the viewer sprite.
static void fsDrawBubble(TFT_eSprite &g, const FSMsg &m, int yTop, int idx, int mode) {
  int bx = FS_BM, bw = SCRW - 2 * FS_BM;
  String lines[24];
  int nl = fsWrap(m.msg, bw - 2 * FS_BP, lines, 24);
  if (nl < 1) nl = 1;
  int bh = fsBubbleH(m, mode);

  if (mode == FS_MESSAGES) {
    bool own = (m.user == fsUser());
    uint16_t fill = own ? COL_SEL : COL_ACCENT;
    g.fillRoundRect(bx, yTop, bw, bh, 8, fill);
    g.drawRoundRect(bx, yTop, bw, bh, 8, theme.neon(idx * 2, COL_DIM));
    int tx = bx + FS_BP, ty = yTop + FS_BP;
    g.setTextDatum(TL_DATUM);
    g.setTextColor(COL_DIM, fill);
    g.drawString(own ? String("You") : m.user, tx, ty, 2);
    ty += FS_LH;
    g.setTextColor(COL_FG, fill);
    for (int i = 0; i < nl; i++) { g.drawString(lines[i], tx, ty, 2); ty += FS_LH; }
    return;
  }

  bool showComments = (mode == FS_FEED || mode == FS_MYPOSTS);
  uint16_t fill = COL_ACCENT;
  g.fillRoundRect(bx, yTop, bw, bh, 8, fill);
  g.drawRoundRect(bx, yTop, bw, bh, 8, theme.neon(idx * 2, m.flipped ? COL_SEL : COL_DIM));
  int tx = bx + FS_BP, ty = yTop + FS_BP;
  g.setTextDatum(TL_DATUM);
  g.setTextColor(COL_FG, fill);
  g.drawString(m.user, tx, ty, 2);
  g.setTextDatum(TR_DATUM);
  g.setTextColor(COL_DIM, fill);
  g.drawString(m.date, bx + bw - FS_BP, ty, 2);
  g.setTextDatum(TL_DATUM);
  ty += FS_LH;
  g.setTextColor(COL_FG, fill);
  for (int i = 0; i < nl; i++) { g.drawString(lines[i], tx, ty, 2); ty += FS_LH; }
  ty += 6;
  g.setTextColor(COL_FG, fill);
  g.drawString((m.flipped ? String("* ") : String("")) + m.flips + " flips", tx, ty, 2);
  if (showComments) {
    g.setTextDatum(TR_DATUM);
    g.drawString(String(m.comments) + " comments", bx + bw - FS_BP, ty, 2);
    g.setTextDatum(TL_DATUM);
  }
}

// forward decls (mutually recursive: feed action popup opens the comments viewer)
static FSVResult fsViewer(FSMsg *arr, int n, const String &title, int mode, uint32_t ctxPost, int series);
static void fsCommentsScreen(uint32_t postId);

static void fsActionPopup(FSMsg &m) {
  int bw = 264, bh = 252, bx = (SCRW - bw) / 2, by = (SCRH - bh) / 2;
  int byy = by + 44, bhh = 44, gap = 8;
  for (;;) {
    const char *labels[4] = { m.flipped ? "Unflip" : "Flip", "View Comments", "Add Comment", "Close" };
    tft->fillRoundRect(bx, by, bw, bh, 10, COL_ACCENT);
    tft->drawRoundRect(bx, by, bw, bh, 10, theme.neon(2, COL_SEL));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(m.user, bx + bw / 2, by + 22, 2);
    for (int i = 0; i < 4; i++) {
      int yy = byy + i * (bhh + gap);
      uint16_t f = (i == 0 && m.flipped) ? COL_SEL : COL_BG;
      tft->fillRoundRect(bx + 16, yy, bw - 32, bhh, 6, f);
      tft->drawRoundRect(bx + 16, yy, bw - 32, bhh, 6, theme.neon(i, COL_DIM));
      tft->setTextColor(COL_FG, f);
      tft->drawString(labels[i], bx + bw / 2, yy + bhh / 2, 2);
    }
    tft->setTextDatum(TL_DATUM);

    uint16_t tx, ty;
    if (!waitTap(tx, ty)) continue;
    if ((int)tx < bx || (int)tx > bx + bw || (int)ty < by || (int)ty > by + bh) return;  // outside = close
    int i = ((int)ty - byy) / (bhh + gap);
    if (i < 0 || i > 3 || (((int)ty - byy) % (bhh + gap)) > bhh) continue;
    if (i == 0) { fsFlip(m); return; }
    if (i == 1) { fsCommentsScreen(m.id); return; }
    if (i == 2) { fsAddComment(m.id); m.comments++; return; }
    return;
  }
}

// Smooth-scrolling message viewer (feed or comments) with momentum + footer.
// FEED footer: [< Prev][+ New Post][Next >] -> returns FSV_PREV / FSV_NEXT / FSV_BACK.
// COMMENTS footer: [+ Add Comment]          -> returns FSV_BACK.
static FSVResult fsViewer(FSMsg *arr, int n, const String &title, int mode, uint32_t ctxPost, int series) {
  const int SPR_H = SCRH - HDRH - NAVH;
  int top[FS_MAX], hh[FS_MAX], total;
  auto relayout = [&]() {
    total = FS_BM;
    for (int i = 0; i < n; i++) { hh[i] = fsBubbleH(arr[i], mode); top[i] = total; total += hh[i] + FS_BM; }
  };
  relayout();

  tft->fillScreen(COL_BG);
  drawHeader(title, true);

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  if (spr.createSprite(SCRW, SPR_H) == nullptr) {     // needs PSRAM (~250 KB)
    statusLine("Not enough memory for this view.", TFT_RED);
    uint16_t a, b; waitTap(a, b);
    return FSV_BACK;
  }

  auto footer = [&]() {
    if (mode == FS_FEED)          drawNav("< Prev", "+ New Post", "Next >");
    else if (mode == FS_COMMENTS) drawNav("", "+ Add Comment", "");
    else if (mode == FS_MESSAGES) drawNav("", "+ Send Message", "");
    else                          drawNav("", "", "");   // My Posts: read-only
  };

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > SPR_H ? total - SPR_H : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    spr.fillSprite(COL_BG);
    for (int i = 0; i < n; i++) {
      int y = top[i] - (int)scroll;
      if (y + hh[i] < 0 || y > SPR_H) continue;
      fsDrawBubble(spr, arr[i], y, i, mode);
    }
    if (n == 0) { spr.setTextColor(COL_DIM, COL_BG); spr.setTextDatum(TL_DATUM);
                  spr.drawString(mode == FS_COMMENTS ? "No comments yet."
                               : mode == FS_MESSAGES ? "No messages yet." : "No posts.", 12, 10, 2); }
    spr.pushSprite(0, HDRH);
  };

  footer();
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { spr.deleteSprite(); return FSV_BACK; }
        if ((int)pY >= SCRH - NAVH) {                     // footer nav bar
          int nh = navHit(pX, pY);
          if (mode == FS_FEED) {
            if (nh == 0) { spr.deleteSprite(); return FSV_PREV; }
            if (nh == 2) { spr.deleteSprite(); return FSV_NEXT; }
            if (nh == 1) { fsPost(); n = fsLoadFeed(arr, series); relayout(); scroll = 0;
                           tft->fillScreen(COL_BG); drawHeader(title, true); footer(); }
          } else if (mode == FS_COMMENTS) {
            if (nh == 1) { fsAddComment(ctxPost); n = fsLoadComments(arr, ctxPost); relayout(); scroll = 0;
                           tft->fillScreen(COL_BG); drawHeader(title, true); footer(); }
          } else if (mode == FS_MESSAGES) {
            if (nh == 1) {                                 // Send Message
              char b[256] = {0};
              if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Message:", false) && strlen(b))
                fsSendMessage(g_msgPeer, String(b));
              n = fsLoadMessages(arr, g_msgPeer); relayout(); scroll = 0;
              tft->fillScreen(COL_BG); drawHeader(title, true); footer();
            }
          }
          need = true;
        } else if ((int)pY >= HDRH) {                      // tapped a bubble
          int fy = (int)pY - HDRH + (int)scroll;
          int idx = -1;
          for (int i = 0; i < n; i++) if (fy >= top[i] && fy < top[i] + hh[i]) { idx = i; break; }
          if (idx >= 0 && (mode == FS_FEED || mode == FS_MYPOSTS)) {   // open the post actions
            fsActionPopup(arr[idx]);
            tft->fillScreen(COL_BG); drawHeader(title, true); footer();
            need = true;
          }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }

    wasDown = down;
    if (need) render();
    delay(12);
  }
}

static void fsCommentsScreen(uint32_t postId) {
  static FSMsg cm[FS_MAX];
  tft->fillScreen(COL_BG); drawHeader("Comments", true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  int k = fsLoadComments(cm, postId);
  fsViewer(cm, k, "Comments", FS_COMMENTS, postId, 0);
}

static void feedScreen() {
  static FSMsg feed[FS_MAX];
  int series = 1;
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Feed", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading feed...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsLoadFeed(feed, series);
    FSVResult r = fsViewer(feed, n, String("Feed  p") + series, FS_FEED, 0, series);
    if (r == FSV_BACK) return;
    if (r == FSV_NEXT) series++;
    else if (r == FSV_PREV && series > 1) series--;
  }
}

// A single DM thread with `peer` — chat bubbles + "+ Send Message" footer.
static void messagesThreadScreen(const String &peer) {
  static FSMsg msgs[FS_MAX];
  g_msgPeer = peer;
  tft->fillScreen(COL_BG); drawHeader(String("@") + peer, true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  int n = fsLoadMessages(msgs, peer);
  fsViewer(msgs, n, String("@") + peer, FS_MESSAGES, 0, 0);
}

// Messages — conversation list (+ "New Message"). Tap a user to open the thread.
static void messagesScreen() {
  static String users[40];
  static String rows[41];
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Messages", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsLoadMsgUsers(users, 40);
    int r = 0;
    rows[r++] = "+ New Message";
    for (int i = 0; i < n && r < 41; i++) rows[r++] = users[i];
    int sel = scrollList("Messages", rows, r, true);
    if (sel < 0) return;
    if (sel == 0) {                                    // start a new conversation
      char b[64] = {0};
      if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Message to (username):", false) && strlen(b))
        messagesThreadScreen(String(b));
    } else {
      int idx = sel - 1;
      if (idx >= 0 && idx < n) messagesThreadScreen(users[idx]);
    }
  }
}

// Guard: FlipSocial actions need WiFi + a username.
static bool fsReady() {
  if (WiFi.status() != WL_CONNECTED) {
    msgScreen("FlipSocial", "Connect WiFi first", "Settings > WiFi Setup", TFT_RED);
    return false;
  }
  if (fsUser().length() == 0) {
    msgScreen("FlipSocial", "Set a username first", "Settings > Username", TFT_RED);
    return false;
  }
  return true;
}

// Modal yes/no confirmation (theme + font colours). Returns true on OK.
static bool confirmDialog(const String &title, const String &sub) {
  int bw = 260, bh = 150, bx = (SCRW - bw) / 2, by = (SCRH - bh) / 2;
  tft->fillRoundRect(bx, by, bw, bh, 10, COL_ACCENT);
  tft->drawRoundRect(bx, by, bw, bh, 10, theme.neon(2, COL_SEL));
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  tft->drawString(title, bx + bw / 2, by + 26, 2);
  tft->setTextColor(COL_DIM, COL_ACCENT);
  tft->drawString(sub, bx + bw / 2, by + 50, 2);
  int bhh = 36, byy = by + bh - bhh - 14, halfw = (bw - 48) / 2;
  int cx = bx + 16, okx = bx + bw / 2 + 8;
  tft->fillRoundRect(cx, byy, halfw, bhh, 6, COL_BG);
  tft->drawRoundRect(cx, byy, halfw, bhh, 6, COL_DIM);
  tft->setTextColor(COL_FG, COL_BG);
  tft->drawString("Cancel", cx + halfw / 2, byy + bhh / 2, 2);
  tft->fillRoundRect(okx, byy, halfw, bhh, 6, COL_SEL);
  tft->drawRoundRect(okx, byy, halfw, bhh, 6, COL_DIM);
  tft->setTextColor(COL_FG, COL_SEL);
  tft->drawString("OK", okx + halfw / 2, byy + bhh / 2, 2);
  tft->setTextDatum(TL_DATUM);
  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    return inRect(x, y, okx, byy, halfw, bhh);   // OK button, else Cancel/dismiss
  }
}

// My Posts — the user's own posts, filtered client-side from the feed (no
// dedicated profile endpoint exists in the API). Read-only viewer.
static void myPostsScreen() {
  static FSMsg mine[FS_MAX], buf[FS_MAX];
  tft->fillScreen(COL_BG); drawHeader("My Posts", true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  String me = fsUser();
  int cnt = 0;
  for (int series = 1; series <= 3 && cnt < FS_MAX; series++) {
    int n = fsLoadFeed(buf, series);
    for (int i = 0; i < n && cnt < FS_MAX; i++)
      if (buf[i].user == me) mine[cnt++] = buf[i];
    if (n == 0) break;
  }
  fsViewer(mine, cnt, "My Posts", FS_MYPOSTS, 0, 0);
}

// View Profile — bio, friends count, join date (GET /user/profile/{who}).
static void profileInfoScreen(const String &who) {
  tft->fillScreen(COL_BG); drawHeader("Profile", true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  FSProfile p = fsLoadProfile(who);
  tft->fillRect(0, HDRH, SCRW, SCRH - HDRH, COL_BG);
  if (!p.ok) { statusLine("Could not load profile.", TFT_RED); uint16_t x, y; waitTap(x, y); return; }

  int y = CONTENTY + 14;
  tft->setTextColor(COL_FG, COL_BG); tft->setTextDatum(TL_DATUM);
  tft->drawString(String("@") + who, 14, y, 4); y += 36;
  tft->setTextColor(COL_DIM, COL_BG);
  tft->drawString(String("Friends: ") + p.friends, 14, y, 2); y += 22;
  if (p.joined.length()) { tft->drawString(String("Joined: ") + p.joined, 14, y, 2); y += 26; }
  tft->setTextColor(COL_FG, COL_BG);
  tft->drawString("Bio", 14, y, 2); y += 22;
  String bio = p.bio.length() ? p.bio : String("(no bio yet)");
  String lines[18];
  int nl = fsWrap(bio, SCRW - 28, lines, 18);
  tft->setTextColor(p.bio.length() ? COL_FG : COL_DIM, COL_BG);
  for (int i = 0; i < nl && y < SCRH - 28; i++) { tft->drawString(lines[i], 14, y, 2); y += 20; }
  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

// Friends — smooth-scroll list; tap a friend to remove (confirm).
static void friendsScreen() {
  static String fr[41];
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Friends", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsLoadFriends(fr, 40);
    if (n == 0) {
      static String empty[1]; empty[0] = "(no friends yet)";
      if (scrollList("Friends", empty, 1, false) < 0) return;
      continue;
    }
    int sel = scrollList("Friends", fr, n, true);
    if (sel < 0) return;
    if (sel >= 0 && sel < n && confirmDialog(String("Remove ") + fr[sel] + "?", "")) fsRemoveFriend(fr[sel]);
  }
}

// Profile — full account + social options (from the FlipSocial app).
static void profileScreen() {
  static const char *P[] = { "View Profile", "Edit Bio", "Friends", "Add Friend",
                             "My Posts", "Change Username", "Change Password", "Log Out" };
  static String rows[8];
  for (;;) {
    for (int i = 0; i < 8; i++) rows[i] = P[i];
    String u = fsUser();
    int sel = scrollList(u.length() ? (String("@") + u) : String("Profile"), rows, 8, true);
    if (sel < 0) return;
    switch (sel) {
      case 0: if (fsReady()) profileInfoScreen(fsUser()); break;
      case 1: if (fsReady()) {                                   // Edit Bio
                FSProfile p = fsLoadProfile(fsUser());
                char b[160] = {0}; strncpy(b, p.bio.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Edit Bio:", false)) {
                  bool ok = fsChangeBio(String(b));
                  msgScreen("Bio", ok ? "Bio updated" : "Update failed", "", ok ? COL_OK : TFT_RED);
                }
              } break;
      case 2: if (fsReady()) friendsScreen(); break;
      case 3: if (fsReady()) {                                   // Add Friend
                char b[64] = {0};
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Add friend (username):", false)
                    && strlen(b)) {
                  bool ok = fsAddFriend(String(b));
                  msgScreen("Add Friend", ok ? "Request sent" : "Failed", String(b), ok ? COL_OK : TFT_RED);
                }
              } break;
      case 4: if (fsReady()) myPostsScreen(); break;
      case 5: { char b[64] = {0}; strncpy(b, u.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Username:", false))
                  credSet("user", String(b)); } break;
      case 6: { char b[64] = {0};
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Password:", true))
                  credSet("pass", String(b)); } break;
      case 7: if (confirmDialog("Log out?", "Clears saved login")) {
                credSet("user", ""); credSet("pass", ""); return;
              } break;
    }
  }
}

// Actions for a user found via Explore: view profile, add friend, message.
static void exploreUserScreen(const String &who) {
  static String rows[3];
  for (;;) {
    rows[0] = "View Profile";
    rows[1] = "Add Friend";
    rows[2] = "Message";
    int sel = scrollList(String("@") + who, rows, 3, true);
    if (sel < 0) return;
    if (sel == 0) profileInfoScreen(who);
    else if (sel == 1) {
      bool ok = fsAddFriend(who);
      msgScreen("Add Friend", ok ? "Request sent" : "Failed", who, ok ? COL_OK : TFT_RED);
    } else if (sel == 2) {
      messagesThreadScreen(who);
    }
  }
}

// Explore — search users by keyword, then act on a result.
static void exploreScreen() {
  static String users[40], rows[41];
  char kw[64] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, kw, sizeof(kw), "Search users:", false) || strlen(kw) == 0)
    return;
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Explore", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Searching...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsExplore(String(kw), users, 40);
    int r = 0;
    rows[r++] = "+ New Search";
    for (int i = 0; i < n && r < 41; i++) rows[r++] = users[i];
    int sel = scrollList(String("Explore: ") + kw, rows, r, true);
    if (sel < 0) return;
    if (sel == 0) {                                    // new search
      kw[0] = 0;
      if (!touchKeyboardInput(*tft, COL_FG, COL_BG, kw, sizeof(kw), "Search users:", false) || strlen(kw) == 0)
        return;
      continue;
    }
    int idx = sel - 1;
    if (idx >= 0 && idx < n) exploreUserScreen(users[idx]);
  }
}

// ── Main menu (ESP32_Bible-style large rounded buttons) ───────────────────────
static const char *MENU_ITEMS[] = { "Feed", "New Post", "Messages", "Explore", "Profile", "Settings" };
static const int    MENU_COUNT  = 6;
static const int    MENU_MARGIN = 16;
static const int    MENU_TOP    = CONTENTY + 12;
static const int    MENU_GAP    = 12;
static int menuBtnH() {
  int avail = SCRH - MENU_TOP - 12;
  return (avail - (MENU_COUNT - 1) * MENU_GAP) / MENU_COUNT;
}
static int menuBtnY(int i) { return MENU_TOP + i * (menuBtnH() + MENU_GAP); }
static int menuButtonAt(uint16_t x, uint16_t y) {
  if ((int)x < MENU_MARGIN || (int)x >= SCRW - MENU_MARGIN) return -1;
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int by = menuBtnY(i);
    if ((int)y >= by && (int)y < by + bh) return i;
  }
  return -1;
}

static void drawMenu() {
  tft->fillScreen(COL_BG);
  drawHeader("FlipSocial", false);
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = menuBtnY(i);
    tft->fillRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, COL_ACCENT);
    tft->drawRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, theme.neon(i * 3, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    tft->drawString(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 4);
  }
  tft->setTextDatum(TL_DATUM);
}

static void openMenuItem(int i) {
  switch (i) {
    case 0: if (fsReady()) feedScreen();     break;     // Feed
    case 1: if (fsReady()) fsPost();         break;     // New Post
    case 2: if (fsReady()) messagesScreen(); break;     // Messages
    case 3: if (fsReady()) exploreScreen();  break;     // Explore
    case 4: profileScreen();             break;
    case 5: settingsFlow();              break;
    default: break;
  }
  drawMenu();
}

static bool mainMenuStart(ViewManager *viewManager) {
  drawMenu();
  return true;
}

static void mainMenuRun(ViewManager *viewManager) {
  static bool wasDown = false;
  TouchInput *t = viewManager->getInputManager()->getTouch();
  bool down = t->isPressed();
  if (down && !wasDown) {                              // fresh tap (press edge)
    uint16_t x = t->x(), y = t->y();
    int btn = menuButtonAt(x, y);
    if (btn >= 0) openMenuItem(btn);
  }
  wasDown = down;

  // Idle refresh: header WiFi icon + battery. Repaint promptly when the WiFi
  // state changes (connecting -> connected, drops, etc.), else periodically.
  static uint32_t lastRefresh = 0;
  static int lastStatus = -2;
  static bool lastConn = false;
  if (WiFi.status() != lastStatus || g_wifiConnecting != lastConn || millis() - lastRefresh > 4000) {
    lastRefresh = millis();
    bool statusChanged = (WiFi.status() != lastStatus);
    lastStatus = WiFi.status();
    lastConn   = g_wifiConnecting;
    if (statusChanged) drawMenu();       // SSID text on the menu may change too
    else               drawHeaderStatus();
  }
}

static const PROGMEM View mainMenuView = View("MainMenu", mainMenuRun, mainMenuStart, nullptr);

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  randomSeed(esp_random());
#ifndef DEVELOPER
  esp_log_level_set("*", ESP_LOG_NONE);
#endif

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);
  Serial.println(F("[Pancake] FlipSocial starting..."));

  // Backlight off during init (PWM).
  pinMode(TFT_BL, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 0);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);
#endif

  // SD (shared FSPI bus on ESP32-C5) — must be up before ViewManager (Storage).
#ifdef HAS_C5_SD
  sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  delay(100);
  if (!SD.begin(SD_CS, sharedSPI)) Serial.println(F("[Pancake] SD init failed"));
  else Serial.println(F("[Pancake] SD OK"));
#else
  if (!SD.begin(SD_CS)) Serial.println(F("[Pancake] SD init failed"));
#endif

  // SPIFFS for settings + credentials (format on first boot).
  if (!SPIFFS.begin(true)) Serial.println(F("[Pancake] SPIFFS mount failed"));
  else                     Serial.println(F("[Pancake] SPIFFS OK"));

#ifdef HAS_PSRAM
  if (!psramInit()) Serial.println(F("[Pancake] PSRAM unavailable"));
#endif

  // Capacitive touch (also does Wire.begin on the shared I2C bus).
  ft6336_init();
  battInit();                          // MAX17048 fuel gauge on the same I2C bus

  // Load persisted theme/accent/font/brightness before anything draws.
  theme.load();

  // ViewManager owns the panel (Draw) and touch (InputManager).
  vm    = new ViewManager(PancakeConfig);
  tft   = vm->getDraw()->display->getTFT();
  touch = vm->getInputManager()->getTouch();
  applyThemeToViewManager();

  // Backlight on at the saved brightness.
  applyBrightness();

  // WiFi: capture disconnect reasons for diagnostics.
  WiFi.onEvent(wifiEvent);
  WiFi.mode(WIFI_STA);

  // Show the main menu immediately, then connect to saved WiFi in the background
  // (header icon + LED report progress).
  vm->add(&mainMenuView);
  vm->set("MainMenu");
  wifiBgBegin();
  drawHeaderStatus();                  // show the "connecting" (yellow) icon at once

  Serial.println(F("[Pancake] Ready."));
}

void loop() {
  vm->run();
  wifiBgTick();                        // advance the background WiFi connect

  // Activity LED mirrors the connecting state (on while scanning/associating).
  // Note: FlipSocial screens block loop() and drive the LED themselves via fsRequest.
  static bool ledState = false;
  if (g_wifiConnecting != ledState) { ledState = g_wifiConnecting; ledSet(ledState); }

  delay(5);
}
