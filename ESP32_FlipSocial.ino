/* ============================================================================
   ESP32 FlipSocial — Pancake (ESP32-C5, ST7796 320x480, FT6336 capacitive touch)
   ============================================================================
   Standalone touch firmware. list-menu UI shell (list menus, header
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

// Picoware core (panel init, touch, HTTP). FlipSocial is a native H4W9-style app
// in this sketch, so Picoware's own flip_social views are not used.
#include "src/Picoware/internal/boards.hpp"
#include "src/Picoware/internal/gui/draw.hpp"
#include "src/Picoware/internal/system/input.hpp"
#include "src/Picoware/internal/system/http.hpp"
#include "src/Picoware/internal/system/view.hpp"
#include "src/Picoware/internal/system/view_manager.hpp"
using namespace Picoware;

// Globals
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

// Panel size comes from the board block in configs.h. Rotation is left at the
// power-on default (0 = portrait), so these map straight onto the panel:
// Pancake ST7796 = 320x480, V8 ILI9341 = 240x320. The layout below is derived
// from these, so it reflows per board.
static const int SCRW = TFT_WIDTH;
static const int SCRH = TFT_HEIGHT;

// Shell layout — matches H4W9 (header 28, nav 28, list rows 34).
static const int HDRH     = 28;
static const int NAVH     = 28;
// The settings list is drawn at fixed offsets and does not scroll, so the rows
// have to fit the panel: at 34 px they run off the bottom of the V8's 320 px
// screen. 26 px still clears the 22 px chips and the 16 px font.
#ifdef MARAUDER_V8
static const int ITEMH    = 26;
#else
static const int ITEMH    = 34;
#endif
static const int CONTENTY = HDRH;

// FlipSocial message + viewer result (defined here so Arduino's auto-generated
// prototypes, inserted above the first function, can see these types).
struct FSMsg { uint32_t id; String user, msg, date; int flips, comments; bool flipped; };
struct FSProfile { String bio, joined; int friends; bool ok; };
enum FSVResult { FSV_BACK, FSV_PREV, FSV_NEXT };
enum FSCred { FSC_OK, FSC_NOUSER, FSC_BADPASS, FSC_EMPTY, FSC_ERR };

#ifndef HAS_CAP_TOUCH
// Resistive touch calibration (V8). Capacitive panels report real coordinates
// and need none of this. The 5 uint16 blob is TFT_eSPI's own format; it lives on
// SPIFFS next to the other settings.
static const char *TOUCH_CAL_FILE = "/pico_touch.dat";

static bool touchCalLoad(uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "r");
  if (!f) return false;
  bool ok = (f.read((uint8_t *)cal, sizeof(uint16_t) * 5) == sizeof(uint16_t) * 5);
  f.close();
  return ok;
}
static void touchCalSave(const uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "w");
  if (!f) return;
  f.write((const uint8_t *)cal, sizeof(uint16_t) * 5);
  f.close();
}
// TFT_eSPI's 4-corner wizard. Blocking, and deliberately drawn without the theme
// so it is legible before anything else is up.
static void touchCalRun() {
  uint16_t cal[5];
  tft->fillScreen(TFT_BLACK);
  tft->setTextColor(TFT_WHITE, TFT_BLACK);
  tft->setTextDatum(MC_DATUM);
  tft->drawString("Touch Calibration", SCRW / 2, SCRH / 2 - 24, 4);
  tft->drawString("Tap each corner arrow", SCRW / 2, SCRH / 2 + 6, 2);
  tft->setTextDatum(TL_DATUM);
  delay(1500);
  tft->fillScreen(TFT_BLACK);
  tft->calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 15);
  tft->setTouch(cal);
  touchCalSave(cal);
}
// Load the stored calibration, or run the wizard once on first boot.
static void touchCalInit() {
  uint16_t cal[5];
  if (touchCalLoad(cal)) tft->setTouch(cal);
  else                   touchCalRun();
}
#endif // !HAS_CAP_TOUCH

// Touch helpers
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

// Theme / brightness plumbing
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

// Status LED. Colour-coded by action where the hardware allows:
//   amber = WiFi scan/connect, blue = HTTP fetch, green = success, red = error.
// The public API (ledOff/ledWifi/ledHttp/ledOk/ledErr/ledBlinkOk/ledSet) is the
// same for both backends; only the drive layer differs per board.
#ifdef HAS_ACT_LED
// V8: one blue GPIO LED (active-high). GPIO28 is a strapping pin (pull-up =
// normal SPI boot), so it is handled exactly like the Marauder firmware does:
// a plain digital output — no PWM/LEDC routed onto the strap pin and no pad-hold,
// so a reset always releases it back to the pull-up and boots normally. No colour,
// so every status maps to on/off; success is a brief blink. led_bright 0 = off,
// keeping the Settings LED row functional (as a simple on/off, not a dimmer).
static bool g_actLedReady = false;
static void ledActArm() {
  if (g_actLedReady) return;
  pinMode(ACT_LED_PIN, OUTPUT);
  digitalWrite(ACT_LED_PIN, LOW);
  g_actLedReady = true;
}
static void ledActSet(bool on) {
  ledActArm();
  digitalWrite(ACT_LED_PIN, (on && theme.led_bright > 0) ? HIGH : LOW);
}
static inline void ledOff()  { ledActSet(false); }
static inline void ledWifi() { ledActSet(true); }
static inline void ledHttp() { ledActSet(true); }
static inline void ledOk()   { ledActSet(true); }
static inline void ledErr()  { ledActSet(true); }
static inline void ledBlinkOk(uint16_t ms = 150) { ledActSet(true); delay(ms); ledActSet(false); }
static void ledSet(bool on) { ledActSet(on); }

#else
// Pancake: onboard addressable RGB LED (WS2812-style).
#ifdef RGB_BUILTIN
  #define PW_RGB_PIN RGB_BUILTIN
#else
  #define PW_RGB_PIN LED_BUILTIN
#endif
// Colours are full-intensity hues; ledRGB scales them by the LED-brightness
// setting (0..20, where 0 = off). Default 4 keeps the LED gentle.
//
// The RGB LED is WS2812-style: consecutive frames must be separated by a reset
// gap (>50us idle low) or the LED latches the first frame and passes the next
// one down the chain — so a colour followed immediately by off stays lit. Hold
// off before every frame so back-to-back writes always land.
static inline void ledGap() { delayMicroseconds(300); }
static void ledRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t s = theme.led_bright;
  ledGap();
  rgbLedWrite(PW_RGB_PIN, (uint8_t)((uint16_t)r * s / 20),
                          (uint8_t)((uint16_t)g * s / 20),
                          (uint8_t)((uint16_t)b * s / 20));
}
static inline void ledOff()  { ledGap(); rgbLedWrite(PW_RGB_PIN, 0, 0, 0); }  // truly off
static inline void ledWifi() { ledRGB(255, 150, 0); }  // amber — scanning / connecting
static inline void ledHttp() { ledRGB(0,   80, 255); } // blue  — HTTP request in flight
static inline void ledOk()   { ledRGB(0,  255,   0); } // green — success
static inline void ledErr()  { ledRGB(255,  0,   0); } // red   — error
// Success blink for actions that finish too fast for a bare ledOk() to be seen.
static inline void ledBlinkOk(uint16_t ms = 150) { ledOk(); delay(ms); ledOff(); }
// Back-compat shim: old on/off calls map to the WiFi (amber) colour.
static void ledSet(bool on) { if (on) ledWifi(); else ledOff(); }
#endif // HAS_ACT_LED

// FlipSocial credentials (SPIFFS: /pico_user.json)
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

// Saved WiFi networks (SPIFFS: /pico_wifi.json = {"nets":[{"s","p"}]})
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

// Battery fuel gauge (MAX17048, I2C 0x36, shared bus)
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

// Rendering helpers
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
    tft->drawString(pct, rx, HDRH / 2, 1);             // small (font 1) like H4W9
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

// Crisp vector chevron "<"/">" (solid triangle) — matches H4W9 selectors.
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

// H4W9-style header: optional back box with chevron (top-left), centred
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

// Footer nav bar (H4W9-style): up to three labelled rounded buttons in thirds.
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

// One list row: fill, left text, optional right chevron, divider. Divider/chevron
// follow the theme (neon rainbow on the Neon theme, else edge/dim).
static void drawListRow(int y, const String &text, bool sel, bool arrow) {
  uint16_t bgc = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  tft->fillRect(0, y, SCRW, ITEMH, bgc);
  tft->setTextColor(COL_FG, bgc);
  tft->setTextDatum(ML_DATUM);
  tft->drawString(text, 12, y + ITEMH / 2, 2);
  if (arrow) drawChevron(SCRW - 26, y, 16, ITEMH, true, theme.neon(seed, COL_DIM));
  tft->drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  tft->setTextDatum(TL_DATUM);
}

// Sprite version of a list row (for flicker-free momentum scrolling). `seed` is
// the row index, used to vary the neon hue down the list.
static void drawRowSprite(TFT_eSprite &spr, int y, const String &text, bool arrow, int seed) {
  spr.fillRect(0, y, SCRW, ITEMH, COL_BG);
  spr.setTextColor(COL_FG, COL_BG);
  spr.setTextDatum(ML_DATUM);
  spr.drawString(text, 12, y + ITEMH / 2, 2);
  if (arrow) {
    int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
    spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  }
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

// Scrollbar drawn into a sprite: track + thumb at the right edge. The thumb
// follows the theme (neon hue on the Neon theme, else dim).
// `viewH` = visible height, `total` = content height, `scroll` = current offset.
static void sprScrollBar(TFT_eSprite &spr, int viewH, int total, float scroll) {
  if (total <= viewH) return;
  const int bw = 4, bx = SCRW - bw - 1;
  spr.fillRect(bx, 0, bw, viewH, theme.edge());
  int thumbH = viewH * viewH / total; if (thumbH < 14) thumbH = 14;
  int maxS = total - viewH;
  int thumbY = (maxS > 0) ? (int)((scroll / (float)maxS) * (viewH - thumbH)) : 0;
  // Thumb hue tracks the scroll position (neon rainbow on the Neon theme).
  spr.fillRect(bx, thumbY, bw, thumbH, theme.neon(thumbY / 12, COL_DIM));
}

// scrollList return sentinels for footer-button taps (Back is SL_BACK).
static const int SL_BACK = -1, SL_F0 = -2, SL_F1 = -3, SL_F2 = -4;

// Momentum-scrolling list of string rows with a right-edge scrollbar. Optional
// footer nav bar (pass labels): a footer tap returns SL_F0/SL_F1/SL_F2, Back
// returns SL_BACK, and a row tap returns its index.
static int scrollList(const String &title, String *rows, int n, bool arrow,
                      const char *fL = nullptr, const char *fM = nullptr, const char *fR = nullptr) {
  bool hasFooter = (fL && fL[0]) || (fM && fM[0]) || (fR && fR[0]);
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY - (hasFooter ? NAVH : 0);
  int total = n * ITEMH;
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  if (hasFooter) drawNav(fL ? fL : "", fM ? fM : "", fR ? fR : "");

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
        drawRowSprite(spr, y, rows[i], arrow, i);
      }
      sprScrollBar(spr, CH, total, scroll);
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
        if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); return SL_BACK; }
        if (hasFooter && (int)pY >= SCRH - NAVH) {          // footer button
          int nh = navHit(pX, pY);
          if (haveSpr) spr.deleteSprite();
          return nh == 0 ? SL_F0 : nh == 2 ? SL_F2 : SL_F1;
        }
        if ((int)pY >= CY && (int)pY < CY + CH) {
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

// Settings chip rows: label + [<] value [>] (or [-] value [+])
// Settings rows (H4W9 choiceRow layout)
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

// Centred message screen with a Back header: headline `a` + optional detail `b`
// (word-wrapped so long failure reasons stay readable). Blocks for a tap.
static void msgScreen(const char *title, const String &a, const String &b, uint16_t col) {
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  tft->setTextColor(col, COL_BG);
  tft->setTextDatum(MC_DATUM);
  tft->drawString(a, SCRW / 2, SCRH / 2 - 20, 2);
  if (b.length()) {
    tft->setTextColor(COL_DIM, COL_BG);
    // Greedy word-wrap to the screen width (no dependency on fsWrap).
    int y = SCRH / 2 + 6, maxW = SCRW - 24;
    String line = "", rest = b;
    while (rest.length() && y < SCRH - 20) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (tft->textWidth(cand.c_str(), 2) <= maxW) { line = cand; }
      else { tft->drawString(line, SCRW / 2, y, 2); y += 20; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 20) tft->drawString(line, SCRW / 2, y, 2);
  }
  tft->setTextDatum(TL_DATUM);
  uint16_t x, y2; waitTap(x, y2);
}

// WiFi
// Last STA disconnect reason (WIFI_REASON_*): 15 = 4-way handshake timeout
// (usually wrong password), 201 = no AP found (band/channel), 205 = conn fail.
static volatile int g_wifiReason = 0;
static volatile int g_wifiEvt = -1;   // last Arduino WiFi event id (-1 = none seen)
static bool g_manualDisconnect = false;   // user tapped Disconnect — don't auto-reconnect
static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  g_wifiEvt = (int)event;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    g_wifiReason = info.wifi_sta_disconnected.reason;
}

// Poll for association up to timeoutMs, animating a "connecting..." line at
// `spinnerY`. Tapping the screen cancels (returns false).
static bool waitConnect(uint32_t timeoutMs, int spinnerY) {
  uint32_t start = millis(), lastAnim = 0;
  bool wasDown = touch->isPressed();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) return false;    // tap to cancel
    wasDown = down;
    if (millis() - lastAnim > 350) {
      lastAnim = millis();
      String d = "connecting";
      for (int i = 0; i < (dots = (dots + 1) % 4); i++) d += ".";
      tft->fillRect(0, spinnerY, SCRW, 20, COL_BG);
      tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
      tft->drawString(d, SCRW / 2, spinnerY + 8, 2);
      tft->setTextDatum(TL_DATUM);
    }
    delay(30);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Blocking connect with a clean loading screen. FlipperHTTP's ESP32-C5 approach:
// setBandMode(AUTO) + a plain WiFi.begin() — no scan / BSSID pin / radio cycle.
static bool connectWiFi(const String &ssid, const String &pass) {
  g_wifiConnecting = true;
  g_manualDisconnect = false;                // an explicit connect re-enables auto-reconnect
  ledWifi();
  g_wifiReason = 0;
  g_wifiEvt = -1;
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.scanDelete();                         // free any prior scan (harmless if none)
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);     // dual-band C5: auto-select the band
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(false);

  tft->fillScreen(COL_BG);
  drawHeader("WiFi", true);
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  tft->drawString("Connecting to", SCRW / 2, SCRH / 2 - 22, 2);
  tft->setTextColor(COL_FG, COL_BG);
  tft->drawString(String("\"") + ssid + "\"", SCRW / 2, SCRH / 2 + 4, 4);
  tft->setTextDatum(TL_DATUM);

  bool ok = waitConnect(12000, SCRH / 2 + 34);
  g_wifiConnecting = false;
  ledOff();
  return ok;
}

// Connect to a saved network — just its stored password, no scan.
static bool connectSaved(const String &ssid) {
  return connectWiFi(ssid, wifiPassFor(ssid));
}

// Background (re)connect — NON-BLOCKING so the menu stays responsive. An async
// scan orders the saved networks by signal strength (closest first); then it
// connects to each in turn (setBandMode(AUTO) + WiFi.begin(), poll, next on
// timeout). Driven from loop() via wifiBgTick(); retriggered by loop() on loss.
enum WbState { WB_IDLE, WB_SCAN, WB_CONNECT, WB_DONE };
static WbState  g_wb  = WB_IDLE;
static uint32_t g_wbT = 0;
static String   g_wbSs[WIFI_MAX_SAVED], g_wbPp[WIFI_MAX_SAVED];
static int      g_wbN = 0, g_wbIdx = 0;

static void wifiBgTry() {                     // begin() on the current saved network
  g_wifiReason = 0; g_wifiEvt = -1;
  WiFi.begin(g_wbSs[g_wbIdx].c_str(), g_wbPp[g_wbIdx].c_str());
  WiFi.setAutoReconnect(false);
  g_wbT = millis();
}

static void wifiBgBegin() {
  g_wbN = wifiLoad(g_wbSs, g_wbPp, WIFI_MAX_SAVED);
  if (g_wbN == 0) { g_wb = WB_IDLE; return; }
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true);                    // async — order by RSSI when it completes
  g_wbT = millis();
  g_wb = WB_SCAN;
  g_wifiConnecting = true;
}

static void wifiBgTick() {
  if (g_wb == WB_SCAN) {
    int r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING && millis() - g_wbT < 6000) return;   // wait for the scan (<=6s)
    if (r > 0) {
      // Best RSSI of each saved net (−999 = out of range), then sort desc (closest first).
      int rssi[WIFI_MAX_SAVED];
      for (int i = 0; i < g_wbN; i++) {
        rssi[i] = -999;
        for (int j = 0; j < r; j++)
          if (WiFi.SSID(j) == g_wbSs[i] && WiFi.RSSI(j) > rssi[i]) rssi[i] = WiFi.RSSI(j);
      }
      for (int a = 0; a < g_wbN - 1; a++) {
        int best = a;
        for (int b = a + 1; b < g_wbN; b++) if (rssi[b] > rssi[best]) best = b;
        if (best != a) {
          int tr = rssi[a]; rssi[a] = rssi[best]; rssi[best] = tr;
          String ts = g_wbSs[a]; g_wbSs[a] = g_wbSs[best]; g_wbSs[best] = ts;
          String tp = g_wbPp[a]; g_wbPp[a] = g_wbPp[best]; g_wbPp[best] = tp;
        }
      }
    }
    WiFi.scanDelete();
    g_wbIdx = 0;
    wifiBgTry();
    g_wb = WB_CONNECT;
    return;
  }
  if (g_wb == WB_CONNECT) {
    if (WiFi.status() == WL_CONNECTED) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
    // Try only the two closest saved networks (once each, 8 s), then give up and
    // stay disconnected so the LED isn't lit the whole time we're offline.
    int maxTry = g_wbN < 2 ? g_wbN : 2;
    if (millis() - g_wbT > 8000) {
      if (++g_wbIdx >= maxTry) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
      wifiBgTry();
    }
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

    for (int i = 0; i < rc && i < 41; i++)
      rows[i] = WiFi.SSID(i) + "   ch" + WiFi.channel(i) + "  (" + WiFi.RSSI(i) + ")";

    int sel = scrollList("Scan", rows, rc, true, "Back", "Rescan", "");
    if (sel == SL_BACK || sel == SL_F0) return;        // Back
    if (sel == SL_F1) continue;                        // Rescan

    int idx = sel;
    if (idx < 0 || idx >= rc) continue;
    String ssid = WiFi.SSID(idx);
    char pass[65] = {0};
    String sp = wifiPassFor(ssid);
    if (sp.length()) strncpy(pass, sp.c_str(), sizeof(pass) - 1);
    if (!touchKeyboardInput(*tft, COL_FG, COL_BG, pass, sizeof(pass),
                            (String("Password: ") + ssid).c_str(), true)) continue;
    if (connectWiFi(ssid, pass)) {
      wifiSave(ssid, pass);
      statusLine("Connected!", COL_OK);
      uint16_t a, bb; waitTap(a, bb);
      return;
    }
    statusLine((String("Failed (reason ") + g_wifiReason + "). Tap to re-scan.").c_str(), TFT_RED);
    uint16_t a, bb; waitTap(a, bb);
  }
}

// WiFi Setup: saved networks (tap to connect) with a [Disconnect][Scan][Forget]
// footer. The header chevron is Back; the footer left button disconnects WiFi.
static void wifiSetup() {
  static String rows[WIFI_MAX_SAVED];
  for (;;) {
    String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
    int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
    for (int i = 0; i < n; i++) {
      bool cur = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ss[i]);
      rows[i] = (cur ? String("* ") : String("")) + ss[i];
    }
    int sel = scrollList("WiFi Setup", rows, n, true, "Disconnect", "Scan", n > 0 ? "Forget" : "");
    if (sel == SL_BACK) return;                            // header back
    if (sel == SL_F0) { WiFi.disconnect(true); g_manualDisconnect = true; continue; }   // Disconnect
    if (sel == SL_F1) { scanFlow(); continue; }            // Scan
    if (sel == SL_F2 && n > 0) {                           // Forget — pick a saved net
      static String frows[WIFI_MAX_SAVED];
      for (int i = 0; i < n; i++) frows[i] = ss[i];
      int f = scrollList("Forget", frows, n, true);
      if (f >= 0 && f < n) wifiForget(ss[f]);
      continue;
    }
    if (sel >= 0 && sel < n) connectSaved(ss[sel]);        // tap a saved network
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
    line(String("Channel:      ") + (up ? String(WiFi.channel()) : String("-")));
    line(String("IP:           ") + (up ? WiFi.localIP().toString() : String("-")));
    line(String("RSSI:         ") + (up ? String(WiFi.RSSI()) : String("-")));
    line(String("Free heap:    ") + ESP.getFreeHeap());
    line(String("Free PSRAM:   ") + ESP.getFreePsram());
    drawNav("Disconnect", "HTTP Test", "Reconnect");

    uint16_t x, ty;
    if (!waitTap(x, ty)) continue;
    if (backTapped(x, ty)) return;                 // header back
    int nh = navHit(x, ty);
    if (nh == 0) { WiFi.disconnect(true); g_manualDisconnect = true; continue; }   // Disconnect
    if (nh == 1) httpTest();
    if (nh == 2) {
      String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
      int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
      if (n) connectSaved(ss[0]);
    }
  }
}

// Settings (H4W9 layout: highlight on tap, partial redraw, no flash)
// Theme, Accent, Font Color, Brightness, LED, WiFi, Debug, User, Pass, About
// (+ Calibrate Touch on resistive panels — capacitive needs no calibration).
#ifdef HAS_CAP_TOUCH
static const int SET_N = 10;
#else
static const int SET_N = 11;
#endif
// Value string for the chip rows that need it for hit-testing.
static String setChipVal(int row) {
  switch (row) {
    case 0: return theme.themeName();
    case 1: return theme.accentName();
    case 2: return theme.fontColName();
    case 3: return String(theme.bright + 1) + "/20";
    case 4: return String(theme.led_bright) + "/20";
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
    case 4: drawChipRow(y, "LED",        setChipVal(4), true, s, 0); break;
    case 5: drawInfoRow(y, "WiFi Setup", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""), s); break;
    case 6: drawInfoRow(y, "WiFi Debug", "", s); break;
    case 7: drawInfoRow(y, "Username",   credGet("user"), s); break;
    case 8: drawInfoRow(y, "Password",   credGet("pass").length() ? String("****") : String(""), s); break;
    case 9: drawInfoRow(y, "About",      "", s); break;
#ifndef HAS_CAP_TOUCH
    case 10: drawInfoRow(y, "Calibrate Touch", "", s); break;
#endif
  }
}

// About — app name/version/author, hardware + build detail rows, credits.
static void aboutScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("About", true);

  // Compact spacing so the whole page fits above the "tap to go back" line on the
  // shorter V8 panel (240x320); the Pancake keeps its original roomier layout.
#ifdef MARAUDER_V8
  const int dName = 26, dSub = 17, dAuth = 18, dRule = 6, dRow = 17, dGap = 2, dRule2 = 6, dCred = 16;
  const int valX = 82;   // value column; keeps "XPT2046 resistive" inside 240 px
  const char *credit1 = "App & API by JBlanked";
  const char *credit2 = "Picoware engine";
#else
  const int dName = 32, dSub = 22, dAuth = 24, dRule = 10, dRow = 21, dGap = 4, dRule2 = 8, dCred = 20;
  const int valX = 110;
  const char *credit1 = "FlipSocial app & API by JBlanked";
  const char *credit2 = "jblanked.com/flipper  -  Picoware";
#endif

  int cx = SCRW / 2, y = CONTENTY + 12;

  // Name + version + author (centred, prominent).
  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  tft->drawString(FW_NAME, cx, y, 4); y += dName;
  tft->drawString(String("Version ") + FW_VERSION, cx, y, 2); y += dSub;
  tft->setTextColor(COL_DIM, COL_BG);
  tft->drawString("by " FW_AUTHOR, cx, y, 2); y += dAuth;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(1, theme.edge())); y += dRule;

  // Label : value detail rows.
  tft->setTextDatum(TL_DATUM);
  auto row = [&](const char *label, const String &value) {
    tft->setTextColor(COL_DIM, COL_BG); tft->drawString(label, 16, y, 2);
    tft->setTextColor(COL_FG, COL_BG);  tft->drawString(value, valX, y, 2);
    y += dRow;
  };
  row("Board",   BOARD_NAME);
  row("MCU",     BOARD_MCU);
  row("Display", BOARD_DISPLAY);
  row("Touch",   BOARD_TOUCH);
  // Actual PSRAM size (0 if absent or init failed), rounded to whole MB.
  {
    size_t ps = ESP.getPsramSize();
    if (ps >= 1024 * 1024)  row("PSRAM", String((unsigned)((ps + 512 * 1024) / (1024 * 1024))) + " MB");
    else if (ps > 0)        row("PSRAM", String((unsigned)(ps / 1024)) + " KB");
    else                    row("PSRAM", "None");
  }
  row("Built",   __DATE__);
  row("Commit",  FW_COMMIT);

  y += dGap;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(2, theme.edge())); y += dRule2;
  tft->setTextColor(COL_DIM, COL_BG);
  tft->drawString(credit1, 16, y, 2); y += dCred;
  tft->drawString(credit2, 16, y, 2);

  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
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
    if (backTapped(x, y)) { ledOff(); return; }     // clear any LED preview on exit
    if ((int)y < CONTENTY) continue;
    int row = ((int)y - CONTENTY) / ITEMH;
    if (row < 0 || row >= SET_N) continue;

    // Move the highlight to the tapped row (partial redraw of old + new).
    int old = sel; sel = row;
    if (old != row) { if (old >= 0) drawSettingRow(old, sel); drawSettingRow(row, sel); }
    if (row != 4) ledOff();                         // LED preview only while on the LED row

    int h = (row <= 4) ? chipHit(CONTENTY + row * ITEMH, setChipVal(row), x, y) : -1;
    switch (row) {
      case 0: if (h >= 0) { theme.cycleTheme(h); theme.save(); applyThemeToViewManager(); full(); } break;
      case 1: if (h >= 0) { theme.cycleAccent(h); theme.save(); applyThemeToViewManager(); drawSettingRow(1, sel); } break;
      case 2: if (h >= 0) { theme.cycleFontCol(h); theme.save(); applyThemeToViewManager(); recolor(); } break;
      case 3: if (h == 0 && theme.bright > 0)  theme.bright--;
              else if (h == 1 && theme.bright < 19) theme.bright++;
              if (h >= 0) { theme.save(); applyBrightness(); drawSettingRow(3, sel); } break;
      case 4: if (h == 0 && theme.led_bright > 0)  theme.led_bright--;
              else if (h == 1 && theme.led_bright < 20) theme.led_bright++;
              if (h >= 0) { theme.save(); drawSettingRow(4, sel); }
              ledWifi(); break;                     // live preview at the new brightness
      case 5: wifiSetup(); full(); break;
      case 6: wifiDebug(); full(); break;
      case 7: { char b[64] = {0}; String u = credGet("user"); strncpy(b, u.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "FlipSocial User:", false))
                  credSet("user", String(b));
                full(); } break;
      case 8: { char b[64] = {0}; String p = credGet("pass"); strncpy(b, p.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "FlipSocial Password:", true))
                  credSet("pass", String(b));
                full(); } break;
      case 9: aboutScreen(); full(); break;
#ifndef HAS_CAP_TOUCH
      case 10: touchCalRun(); full(); break;   // resistive drifts — allow a redo
#endif
      default: break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  FlipSocial — native H4W9-style client (chat bubbles, smooth scroll, flips,
//  comments, feed paging). Uses the jblanked.com feed API.
// ═════════════════════════════════════════════════════════════════════════════
#define FS_MAX 40
static const int FS_LH = 18;          // line height (font 2)
static const int FS_BM = 8;           // bubble margin
static const int FS_BP = 8;           // bubble padding
enum { FS_FEED = 0, FS_COMMENTS = 1, FS_MYPOSTS = 2, FS_MESSAGES = 3 };
static String g_msgPeer;              // current message-thread peer (for reload/send)
static bool   g_usingCache = false;   // on-screen data came from the SD cache (offline)

// Max chars the API accepts for a post / comment / message body. Matches
// MAX_MESSAGE_LENGTH in the canonical FlipSocial app, which rejects anything
// longer client-side; sending more gets refused by the server.
#define FS_MAX_MESSAGE 100

static String fsUser() { return credGet("user"); }

// Escape a user-typed string so it is safe inside a JSON string literal. Without
// this a typed " or \ produces malformed JSON and the API rejects the request.
static String jsonEsc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { char u[8]; snprintf(u, sizeof(u), "\\u%04x", (unsigned)(uint8_t)c); o += u; }
        else o += c;
    }
  }
  return o;
}

// auth=false sends only Content-Type — the login/register endpoints take the
// credentials in the payload, and that matches the FlipSocial app's headers.
static String fsRequest(const char *method, const String &url, const String &payload = "",
                        bool auth = true) {
  HTTP http;
  String u = credGet("user");
  String p = credGet("pass");
  const char *hk[] = {"Content-Type", "Username", "Password"};
  const char *hv[] = {"application/json", u.c_str(), p.c_str()};
  ledHttp();                                 // blue while the HTTP request is in flight
  String r = http.request(method, url, payload, hk, hv, auth ? 3 : 1);
  ledOff();
  return r;
}

// Offline cache (SD /fs_cache)
// Feed / comments / messages / profile responses are cached to SD so they can be
// shown when WiFi is down. Sets g_usingCache when the on-screen data is from cache.
static String cacheName(const char *prefix, const String &key) {
  String s = "/fs_cache/"; s += prefix;
  for (size_t i = 0; i < key.length(); i++) {
    char c = key[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    s += ok ? c : '_';
  }
  s += ".json";
  return s;
}
static void cacheWrite(const String &path, const String &content) {
  if (!SD.exists("/fs_cache")) SD.mkdir("/fs_cache");
  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  f.print(content);
  f.close();
}
static String cacheRead(const String &path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}
// GET with caching: on a valid response (contains `marker`) refresh the cache and
// return it; otherwise (offline / failed) fall back to the last cached copy.
static String fsGetCached(const String &url, const String &cachePath, const char *marker) {
  String resp;
  if (WiFi.status() == WL_CONNECTED) resp = fsRequest("GET", url);
  if (resp.indexOf(marker) >= 0) { cacheWrite(cachePath, resp); g_usingCache = false; return resp; }
  g_usingCache = true;
  return cacheRead(cachePath);
}

static bool fsBool(JsonVariant v) {
  if (v.is<bool>()) return v.as<bool>();
  String s = v.as<String>();
  return s == "true" || s == "True" || s == "1";
}

static int fsLoadFeed(FSMsg *arr, int series) {
  String url = "https://www.jblanked.com/flipper/api/feed/20/" + fsUser() + "/" + String(series) + "/max/series/";
  String resp = fsGetCached(url, cacheName("feed_", fsUser() + "_" + series), "\"feed\"");
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
  String resp = fsGetCached(url, cacheName("cmt_", String(postId)), "\"comments\"");
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

// Guard for actions that POST (need a live connection). Shows an offline message.
static bool fsOnline() {
  if (WiFi.status() != WL_CONNECTED) {
    msgScreen("Offline", "Connect to WiFi first", "Settings > WiFi Setup", TFT_RED);
    return false;
  }
  return true;
}

// Every write endpoint answers "[SUCCESS]..." on success (same token the
// FlipSocial app matches on); anything else is a failure worth reporting.
static bool fsOk(const String &r) { return r.indexOf("[SUCCESS]") != -1; }

// Extract a short human-readable reason from a failed API response (like the app).
static String fsReason(const String &resp) {
  if (resp.length() == 0) return "No response from server";
  JsonDocument d;
  if (!deserializeJson(d, resp)) {
    const char *keys[] = { "error", "message", "detail", "reason" };
    // ArduinoJson 7: as<String>() on a missing key yields the literal "null",
    // so skip absent keys explicitly rather than testing the string length.
    for (const char *k : keys) {
      if (d[k].isNull()) continue;
      String s = d[k].as<String>();
      if (s.length()) return s;
    }
  }
  String s = resp; s.trim();
  if (s.startsWith("[")) { int e = s.indexOf(']'); if (e >= 0) { s = s.substring(e + 1); s.trim(); } }
  if (s.length() > 96) s = s.substring(0, 96);
  return s.length() ? s : String("Unknown error");
}

// Reason from the last failed write, so helpers that only return bool can still
// tell the caller's dialog what the server actually said.
static String g_fsErr;
static bool fsOkOr(const String &r) {
  if (fsOk(r)) { g_fsErr = ""; return true; }
  g_fsErr = fsReason(r);
  return false;
}

static int g_commentsAdded = 0;   // # comments added in the current comments view

// The request is synchronous, so only move the local flip state once the server
// has confirmed it — otherwise a rejected flip still reads as applied until the
// next reload.
static void fsFlip(FSMsg &m) {
  if (!fsOnline()) return;
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"post_id\":\"" + m.id + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/flip/", payload);
  if (!fsOk(r)) { ledErr(); msgScreen("Flip", "Failed", fsReason(r), TFT_RED); ledOff(); return; }
  ledBlinkOk();
  m.flipped = !m.flipped;
  m.flips += m.flipped ? 1 : -1;
  if (m.flips < 0) m.flips = 0;
}

// Credential check. The profile endpoint needs no auth, so a wrong password
// still renders a perfectly good profile and only shows up later when a post
// fails. POST /user/login/ is the API's own verification; the response strings
// below are the ones the FlipSocial app matches on. (FSCred is declared at the
// top of the file so Arduino's auto-generated prototypes can see it.)
static FSCred fsCheckCreds() {
  String u = credGet("user"), p = credGet("pass");
  if (!u.length() || !p.length()) return FSC_EMPTY;
  String payload = String("{\"username\":\"") + jsonEsc(u) + "\",\"password\":\"" + jsonEsc(p) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/login/", payload, false);
  if (r.indexOf("[SUCCESS]") >= 0)                     return FSC_OK;
  if (r.indexOf("User not found") >= 0)                return FSC_NOUSER;
  if (r.indexOf("Incorrect password") >= 0)            return FSC_BADPASS;
  if (r.indexOf("Username or password is empty.") >= 0) return FSC_EMPTY;
  return FSC_ERR;
}
static const char *fsCredText(FSCred c) {
  switch (c) {
    case FSC_OK:      return "Credentials verified";
    case FSC_NOUSER:  return "User not found - check Settings";
    case FSC_BADPASS: return "Incorrect password";
    case FSC_EMPTY:   return "Username / password not set";
    default:          return "Could not verify credentials";
  }
}

// Profile / friends API (jblanked user endpoints)
// GET /user/profile/{user}/ -> {"bio","friends_count","date_created"}
static FSProfile fsLoadProfile(const String &who) {
  FSProfile p; p.ok = false; p.friends = 0;
  String url = "https://www.jblanked.com/flipper/api/user/profile/" + who + "/";
  String resp = fsGetCached(url, cacheName("prof_", who), "\"friends_count\"");
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
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"bio\":\"" + jsonEsc(bio) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/change-bio/", payload);
  return fsOkOr(r);
}
// GET /user/friends/{user}/{max}/ -> {"friends":[username, ...]}
static int fsLoadFriends(String *arr, int maxN) {
  String url = "https://www.jblanked.com/flipper/api/user/friends/" + fsUser() + "/" + String(maxN) + "/";
  String resp = fsGetCached(url, cacheName("frnd_", fsUser()), "\"friends\"");
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  JsonArray f = doc["friends"].as<JsonArray>();
  if (f.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : f) { if (n >= maxN) break; arr[n++] = v.as<String>(); }
  return n;
}
static bool fsAddFriend(const String &friendName) {
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"friend\":\"" + jsonEsc(friendName) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/add-friend/", payload);
  return fsOkOr(r);
}
static bool fsRemoveFriend(const String &friendName) {
  if (!fsOnline()) return false;
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"friend\":\"" + jsonEsc(friendName) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/user/remove-friend/", payload);
  return fsOkOr(r);
}

// Messages API (jblanked messages endpoints)
// GET /messages/{me}/get/list/{max}/ -> {"users":[username, ...]} (conversations)
static int fsLoadMsgUsers(String *arr, int maxN) {
  String url = "https://www.jblanked.com/flipper/api/messages/" + fsUser() + "/get/list/" + String(maxN) + "/";
  String resp = fsGetCached(url, cacheName("msglist_", fsUser()), "\"users\"");
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
  String resp = fsGetCached(url, cacheName("msg_", peer), "\"conversations\"");
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
  if (!fsOnline()) return false;
  String payload = String("{\"receiver\":\"") + jsonEsc(peer) + "\",\"content\":\"" + jsonEsc(content) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/messages/" + fsUser() + "/post/", payload);
  bool ok = fsOk(r);
  if (!ok) { ledErr(); msgScreen("Message", "Failed", fsReason(r), TFT_RED); ledOff(); }
  else ledBlinkOk();
  return ok;
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

// Returns true if a comment was successfully added.
static bool fsAddComment(uint32_t postId) {
  if (!fsOnline()) return false;
  char b[FS_MAX_MESSAGE + 1] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Comment:", false)) return false;
  if (strlen(b) == 0) return false;
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"content\":\"" + jsonEsc(b) + "\",\"post_id\":\"" + postId + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/comment/", payload);
  bool ok = fsOk(r);
  if (!ok) { ledErr(); msgScreen("Comment", "Failed", fsReason(r), TFT_RED); ledOff(); }
  else ledBlinkOk();
  return ok;
}

static void fsPost() {
  if (!fsOnline()) return;
  char b[FS_MAX_MESSAGE + 1] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "New Post:", false)) return;
  if (strlen(b) == 0) return;
  tft->fillScreen(COL_BG); drawHeader("New Post", true); statusLine("Posting...");
  String payload = String("{\"username\":\"") + jsonEsc(fsUser()) + "\",\"content\":\"" + jsonEsc(b) + "\"}";
  String r = fsRequest("POST", "https://www.jblanked.com/flipper/api/feed/post/", payload);
  bool ok = fsOk(r);
  if (ok) ledOk(); else ledErr();
  msgScreen("New Post", ok ? "Posted!" : "Failed", ok ? String("") : fsReason(r), ok ? COL_OK : TFT_RED);
  ledOff();
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

// Black or white, whichever is legible on the given RGB565 background.
static uint16_t contrastText(uint16_t c) {
  int r = ((c >> 11) & 0x1F) * 255 / 31;
  int g = ((c >> 5) & 0x3F) * 255 / 63;
  int b = (c & 0x1F) * 255 / 31;
  return ((r * 299 + g * 587 + b * 114) / 1000) > 140 ? 0x0000 : 0xFFFF;
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
    uint16_t body = own ? contrastText(fill) : COL_FG;   // legible on the own-message fill
    g.fillRoundRect(bx, yTop, bw, bh, 8, fill);
    g.drawRoundRect(bx, yTop, bw, bh, 8, theme.neon(idx * 2, own ? COL_SEL : COL_DIM));
    int tx = bx + FS_BP, ty = yTop + FS_BP;
    g.setTextDatum(TL_DATUM);
    g.setTextColor(body, fill);
    g.drawString(own ? String("You") : m.user, tx, ty, 2);
    ty += FS_LH;
    g.setTextColor(body, fill);
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
  g.drawString((m.flipped ? String("* ") : String("")) + m.flips + (m.flips == 1 ? " Flip" : " Flips"), tx, ty, 2);
  if (showComments) {
    g.setTextDatum(TR_DATUM);
    g.drawString(String(m.comments) + (m.comments == 1 ? " Comment" : " Comments"), bx + bw - FS_BP, ty, 2);
    g.setTextDatum(TL_DATUM);
  }
}

// forward decls (mutually recursive: feed action popup opens the comments viewer)
static FSVResult fsViewer(FSMsg *arr, int n, const String &title, int mode, uint32_t ctxPost, int series);
static int fsCommentsScreen(uint32_t postId);   // returns # comments added

static void fsActionPopup(FSMsg &m) {
  // Cap the width to the panel — the 264 px design overflows the V8's 240 px.
  int bw = min(264, SCRW - 24), bh = 252, bx = (SCRW - bw) / 2, by = (SCRH - bh) / 2;
  int byy = by + 44, bhh = 44, gap = 8;
  for (;;) {
    const char *labels[4] = { m.flipped ? "Unflip" : "Flip", "View Comments", "Comment", "Close" };
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
    if (i == 1) { m.comments += fsCommentsScreen(m.id); return; }   // reflect new comments on the post
    if (i == 2) { if (fsAddComment(m.id)) m.comments++; return; }
    return;
  }
}

// Smooth-scrolling message viewer (feed or comments) with momentum + footer.
// FEED footer: [< Prev][+ New Post][Next >] -> returns FSV_PREV / FSV_NEXT / FSV_BACK.
// COMMENTS footer: [Comment]                -> returns FSV_BACK.
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
    if (mode == FS_FEED)          drawNav("< Prev", "New Post", "Next >");
    else if (mode == FS_COMMENTS) drawNav("", "Comment", "");
    else if (mode == FS_MESSAGES) drawNav("< Prev", "Send", "Next >");
    else                          drawNav("< Prev", "", "Next >");   // My Posts: page-only
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
    if (n == 0) {
      spr.setTextDatum(TL_DATUM);
      if (g_usingCache) {
        spr.setTextColor(TFT_RED, COL_BG);
        spr.drawString("Offline - no cached data yet.", 12, 10, 2);
        spr.setTextColor(COL_DIM, COL_BG);
        spr.drawString("Connect to WiFi to load.", 12, 30, 2);
      } else {
        spr.setTextColor(COL_DIM, COL_BG);
        spr.drawString(mode == FS_COMMENTS ? "No comments yet."
                     : mode == FS_MESSAGES ? "No messages yet." : "No posts.", 12, 10, 2);
      }
    }
    sprScrollBar(spr, SPR_H, total, scroll);
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
            if (nh == 1) { if (fsAddComment(ctxPost)) g_commentsAdded++;
                           n = fsLoadComments(arr, ctxPost); relayout(); scroll = 0;
                           tft->fillScreen(COL_BG); drawHeader(title, true); footer(); }
          } else if (mode == FS_MESSAGES) {
            if (nh == 0) { spr.deleteSprite(); return FSV_PREV; }   // previous conversation
            if (nh == 2) { spr.deleteSprite(); return FSV_NEXT; }   // next conversation
            if (nh == 1) {                                 // Send Message
              char b[FS_MAX_MESSAGE + 1] = {0};
              if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Message:", false) && strlen(b))
                fsSendMessage(g_msgPeer, String(b));
              n = fsLoadMessages(arr, g_msgPeer); relayout(); scroll = 0;
              tft->fillScreen(COL_BG); drawHeader(title, true); footer();
            }
          } else if (mode == FS_MYPOSTS) {
            if (nh == 0) { spr.deleteSprite(); return FSV_PREV; }   // previous page
            if (nh == 2) { spr.deleteSprite(); return FSV_NEXT; }   // next page
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
          } else if (idx >= 0 && mode == FS_COMMENTS) {   // tap a comment to flip/unflip it
            fsFlip(arr[idx]);
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

static int fsCommentsScreen(uint32_t postId) {
  static FSMsg cm[FS_MAX];
  g_commentsAdded = 0;
  tft->fillScreen(COL_BG); drawHeader("Comments", true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  int k = fsLoadComments(cm, postId);
  fsViewer(cm, k, g_usingCache ? "Comments [offline]" : "Comments", FS_COMMENTS, postId, 0);
  return g_commentsAdded;
}

static void feedScreen() {
  static FSMsg feed[FS_MAX];
  int series = 1;
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Feed", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading feed...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsLoadFeed(feed, series);
    String title = String("Feed  p") + series + (g_usingCache ? "  [offline]" : "");
    FSVResult r = fsViewer(feed, n, title, FS_FEED, 0, series);
    if (r == FSV_BACK) return;
    if (r == FSV_NEXT) series++;
    else if (r == FSV_PREV && series > 1) series--;
  }
}

// A single DM thread with `peer` — chat bubbles + [< Prev][+ Send][Next >] footer.
// Returns the viewer result so the caller can page between conversations.
static FSVResult messagesThreadScreen(const String &peer) {
  static FSMsg msgs[FS_MAX];
  g_msgPeer = peer;
  tft->fillScreen(COL_BG); drawHeader(String("@") + peer, true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  int n = fsLoadMessages(msgs, peer);
  return fsViewer(msgs, n, String("@") + peer + (g_usingCache ? "  [offline]" : ""), FS_MESSAGES, 0, 0);
}

// Messages — conversation list with a [Back][New Msg] footer. Tap a user to open
// the thread; Prev/Next in the thread page through the conversation list.
static void messagesScreen() {
  static String users[40];
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Messages", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsLoadMsgUsers(users, 40);
    int sel = scrollList(g_usingCache ? "Messages [offline]" : "Messages", users, n, true, "Back", "New Msg", "");
    if (sel == SL_BACK || sel == SL_F0) return;
    if (sel == SL_F1) {                                // start a new conversation
      char b[64] = {0};
      if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Message to (username):", false) && strlen(b)) {
        FSVResult rr;
        do { rr = messagesThreadScreen(String(b)); } while (rr != FSV_BACK);   // no list to page
      }
    } else if (sel >= 0 && sel < n) {                  // open a conversation; page with Prev/Next
      int cur = sel;
      for (;;) {
        FSVResult rr = messagesThreadScreen(users[cur]);
        if (rr == FSV_BACK) break;
        if (rr == FSV_NEXT) cur = (cur + 1) % n;
        else if (rr == FSV_PREV) cur = (cur + n - 1) % n;
      }
    }
  }
}

// Guard: FlipSocial actions need WiFi + a username.
// Just a username set — enough to VIEW cached data offline.
static bool fsUserSet() {
  if (fsUser().length() == 0) {
    msgScreen("FlipSocial", "Set a username first", "Settings > Username", TFT_RED);
    return false;
  }
  return true;
}
// WiFi + username — required to POST (new post, comment, message, friend, search).
static bool fsReady() {
  if (WiFi.status() != WL_CONNECTED) {
    msgScreen("FlipSocial", "Connect WiFi first", "Settings > WiFi Setup", TFT_RED);
    return false;
  }
  return fsUserSet();
}

// Modal yes/no confirmation (theme + font colours). Returns true on OK.
static bool confirmDialog(const String &title, const String &sub) {
  // Cap the width to the panel — the 260 px design overflows the V8's 240 px.
  int bw = min(260, SCRW - 24), bh = 150, bx = (SCRW - bw) / 2, by = (SCRH - bh) / 2;
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
  int series = 1;
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("My Posts", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    String me = fsUser();
    int cnt = 0, n = fsLoadFeed(buf, series);
    for (int i = 0; i < n && cnt < FS_MAX; i++)
      if (buf[i].user == me) mine[cnt++] = buf[i];
    String title = String("My Posts  p") + series + (g_usingCache ? "  [offline]" : "");
    FSVResult r = fsViewer(mine, cnt, title, FS_MYPOSTS, 0, series);
    if (r == FSV_BACK) return;
    if (r == FSV_NEXT) series++;
    else if (r == FSV_PREV && series > 1) series--;
  }
}

// View Profile — bio, friends count, join date (GET /user/profile/{who}).
static void profileInfoScreen(const String &who) {
  tft->fillScreen(COL_BG); drawHeader("Profile", true);
  tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
  tft->drawString("Loading...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
  FSProfile p = fsLoadProfile(who);

  // Only meaningful for your own profile, and only if we can actually reach the
  // API (a cached profile tells us nothing about whether the password works).
  bool self    = who.length() && who == fsUser();
  bool live    = (WiFi.status() == WL_CONNECTED) && !g_usingCache;
  bool checked = false;
  FSCred cred  = FSC_ERR;
  if (self && live) { cred = fsCheckCreds(); checked = true; }

  tft->fillRect(0, HDRH, SCRW, SCRH - HDRH, COL_BG);
  if (g_usingCache) drawHeader("Profile  [offline]", true);
  if (!p.ok) {
    // If the credential check knows why, say that instead of a generic failure.
    if (checked && cred != FSC_OK) statusLine(fsCredText(cred), TFT_RED);
    else statusLine(g_usingCache ? "Offline - no cached profile." : "Could not load profile.", TFT_RED);
    uint16_t x, y; waitTap(x, y); return;
  }

  int y = CONTENTY + 14;
  tft->setTextColor(COL_FG, COL_BG); tft->setTextDatum(TL_DATUM);
  tft->drawString(String("@") + who, 14, y, 4); y += 30;
  if (self) {
    uint16_t c = !checked ? COL_DIM : (cred == FSC_OK ? COL_OK : TFT_RED);
    tft->setTextColor(c, COL_BG);
    tft->drawString(checked ? fsCredText(cred) : "Credentials not checked (offline)", 14, y, 2);
    y += 24;
  } else y += 6;
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
      static String empty[1]; empty[0] = g_usingCache ? "Offline - no cached friends" : "(no friends yet)";
      if (scrollList(g_usingCache ? "Friends [offline]" : "Friends", empty, 1, false) < 0) return;
      continue;
    }
    int sel = scrollList("Friends", fr, n, true);
    if (sel < 0) return;
    if (sel >= 0 && sel < n && confirmDialog(String("Remove ") + fr[sel] + "?", "")) {
      if (!fsRemoveFriend(fr[sel])) msgScreen("Friends", "Remove failed", g_fsErr, TFT_RED);
    }
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
      case 0: if (fsUserSet()) profileInfoScreen(fsUser()); break;   // View — offline ok
      case 1: if (fsReady()) {                                   // Edit Bio
                FSProfile p = fsLoadProfile(fsUser());
                char b[160] = {0}; strncpy(b, p.bio.c_str(), sizeof(b) - 1);
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Edit Bio:", false)) {
                  bool ok = fsChangeBio(String(b));
                  msgScreen("Bio", ok ? "Bio updated" : "Update failed", ok ? String("") : g_fsErr,
                            ok ? COL_OK : TFT_RED);
                }
              } break;
      case 2: if (fsUserSet()) friendsScreen(); break;              // Friends — offline ok
      case 3: if (fsReady()) {                                   // Add Friend
                char b[64] = {0};
                if (touchKeyboardInput(*tft, COL_FG, COL_BG, b, sizeof(b), "Add friend (username):", false)
                    && strlen(b)) {
                  bool ok = fsAddFriend(String(b));
                  msgScreen("Add Friend", ok ? "Request sent" : "Failed",
                            ok ? String(b) : g_fsErr, ok ? COL_OK : TFT_RED);
                }
              } break;
      case 4: if (fsUserSet()) myPostsScreen(); break;              // My Posts — offline ok
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
      msgScreen("Add Friend", ok ? "Request sent" : "Failed", ok ? who : g_fsErr,
                ok ? COL_OK : TFT_RED);
    } else if (sel == 2) {
      FSVResult rr;
      do { rr = messagesThreadScreen(who); } while (rr != FSV_BACK);   // no list to page here
    }
  }
}

// Explore — search users by keyword, then act on a result.
static void exploreScreen() {
  static String users[40];
  char kw[64] = {0};
  if (!touchKeyboardInput(*tft, COL_FG, COL_BG, kw, sizeof(kw), "Search users:", false) || strlen(kw) == 0)
    return;
  for (;;) {
    tft->fillScreen(COL_BG); drawHeader("Explore", true);
    tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
    tft->drawString("Searching...", SCRW / 2, SCRH / 2, 2); tft->setTextDatum(TL_DATUM);
    int n = fsExplore(String(kw), users, 40);
    int sel = scrollList(String("Explore: ") + kw, users, n, true, "Back", "Search", "");
    if (sel == SL_BACK || sel == SL_F0) return;
    if (sel == SL_F1) {                                // new search
      kw[0] = 0;
      if (!touchKeyboardInput(*tft, COL_FG, COL_BG, kw, sizeof(kw), "Search users:", false) || strlen(kw) == 0)
        return;
      continue;
    }
    if (sel >= 0 && sel < n) exploreUserScreen(users[sel]);
  }
}

// Main menu (H4W9-style large rounded buttons)
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
    // V8's buttons are ~34 px tall — font 4 (26 px) crowds them, so use font 2.
#ifdef MARAUDER_V8
    tft->drawString(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 2);
#else
    tft->drawString(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 4);
#endif
  }
  tft->setTextDatum(TL_DATUM);
}

static void openMenuItem(int i) {
  switch (i) {
    case 0: if (fsUserSet()) feedScreen();     break;   // Feed — viewable offline (cache)
    case 1: if (fsReady())   fsPost();         break;   // New Post — needs WiFi
    case 2: if (fsUserSet()) messagesScreen(); break;   // Messages — viewable offline (cache)
    case 3: if (fsReady())   exploreScreen();  break;   // Explore — needs a live search
    case 4: profileScreen();               break;
    case 5: settingsFlow();                break;
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

  // Idle refresh of ONLY the header status corner (WiFi icon + battery). The menu
  // buttons don't depend on WiFi, so never repaint the whole screen here — doing so
  // made the menu flash repeatedly while the background connect cycled states.
  static uint32_t lastRefresh = 0;
  static int lastStatus = -2;
  static bool lastConn = false;
  if (WiFi.status() != lastStatus || g_wifiConnecting != lastConn || millis() - lastRefresh > 4000) {
    lastRefresh = millis();
    lastStatus  = WiFi.status();
    lastConn    = g_wifiConnecting;
    drawHeaderStatus();
  }
}

static const PROGMEM View mainMenuView = View("MainMenu", mainMenuRun, mainMenuStart, nullptr);

// Arduino entry points
void setup() {
  randomSeed(esp_random());
#ifndef DEVELOPER
  esp_log_level_set("*", ESP_LOG_NONE);
#endif

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);
  Serial.println(F("[" BOARD_NAME "] FlipSocial starting..."));

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
  if (!SD.begin(SD_CS, sharedSPI)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
  else Serial.println(F("[" BOARD_NAME "] SD OK"));
#else
  if (!SD.begin(SD_CS)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
#endif

  // SPIFFS for settings + credentials (format on first boot).
  if (!SPIFFS.begin(true)) Serial.println(F("[" BOARD_NAME "] SPIFFS mount failed"));
  else                     Serial.println(F("[" BOARD_NAME "] SPIFFS OK"));

#ifdef HAS_PSRAM
  if (!psramInit()) Serial.println(F("[" BOARD_NAME "] PSRAM unavailable"));
#endif

#ifdef HAS_CAP_TOUCH
  // Capacitive touch (also does Wire.begin on the shared I2C bus).
  ft6336_init();
#else
  // V8 has no I2C touch controller (XPT2046 rides the SPI bus), but the fuel
  // gauge below still needs the I2C bus that ft6336_init() would have opened.
  Wire.begin(I2C_SDA, I2C_SCL, 400000U);
#endif
  battInit();                          // MAX17048 fuel gauge on the same I2C bus

  // Load persisted theme/accent/font/brightness before anything draws.
  theme.load();

  // Put the status LED in a known-off state (also arms the V8's PWM channel).
  ledOff();

  // ViewManager owns the panel (Draw) and touch (InputManager).
#ifdef MARAUDER_V8
  vm    = new ViewManager(MarauderV8Config);
#else
  vm    = new ViewManager(PancakeConfig);
#endif
  tft   = vm->getDraw()->display->getTFT();
  touch = vm->getInputManager()->getTouch();
  applyThemeToViewManager();

  // Backlight on at the saved brightness.
  applyBrightness();

#ifndef HAS_CAP_TOUCH
  // Resistive panel: point TouchInput at TFT_eSPI's XPT2046 reader, then load
  // the stored calibration (or run the wizard). Must come after the backlight is
  // up, or a first-boot user would be tapping an unlit screen.
  if (touch) touch->attachTFT(tft);
  touchCalInit();
#endif

  // WiFi: capture disconnect reasons for diagnostics.
  WiFi.onEvent(wifiEvent);
  WiFi.mode(WIFI_STA);

  // Show the main menu immediately, then connect to saved WiFi in the background
  // (header icon + LED report progress).
  vm->add(&mainMenuView);
  vm->set("MainMenu");
  wifiBgBegin();
  drawHeaderStatus();                  // show the "connecting" (yellow) icon at once

  Serial.println(F("[" BOARD_NAME "] Ready."));
}

void loop() {
  vm->run();
  wifiBgTick();                        // advance the background WiFi connect

  // Reconnect watchdog: ONLY on the drop edge (connected -> lost), make one
  // reconnect pass (the two closest saved nets). If it fails we stay disconnected
  // rather than retrying forever — the LED goes off instead of pulsing amber.
  static bool wasConnected = false;
  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (wasConnected && !nowConnected && !g_manualDisconnect && (g_wb == WB_DONE || g_wb == WB_IDLE)) {
    wifiBgBegin();                      // scans + reconnects to the closest saved network
  }
  wasConnected = nowConnected;

  // Activity LED mirrors the connecting state (on while scanning/associating).
  // Note: FlipSocial screens block loop() and drive the LED themselves via fsRequest.
  static bool ledState = false;
  if (g_wifiConnecting != ledState) { ledState = g_wifiConnecting; ledSet(ledState); }

  delay(5);
}
