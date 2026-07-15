// TouchKeyboard.cpp
// Self-contained touch QWERTY keyboard for Pancake Picoware.
// See TouchKeyboard.h. Uses built-in TFT_eSPI fonts and the FT6336 driver.

#include "TouchKeyboard.h"

#ifdef HAS_TOUCH

#include <string.h>
#include <Arduino.h>

#ifdef HAS_CAP_TOUCH
#  include "ft6336.h"
#else
#  define KB_XPT_THRESHOLD 600
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────────────────────────
static const int KB_ROWS = 5;   // 4 char rows + 1 control row
static const int KB_COLS = 10;

static const char ROW0_ALPHA[] = "1234567890";
static const char ROW1_ALPHA[] = "qwertyuiop";
static const char ROW2_ALPHA[] = "asdfghjkl";
static const char ROW3_ALPHA[] = "zxcvbnm.";   // cols 0-7; cols 8-9 = CAPS key

static const char ROW0_SYM[] = "!@#$%^&*()";
static const char ROW1_SYM[] = "`~-_=+[]{}";
static const char ROW2_SYM[] = "\\|;:'\"<>";
static const char ROW3_SYM[] = ",./?";         // centered

// Control row (row 4) columns:
//  0 = CANCEL   1 = SYM/ABC   2..7 = SPACE (6 cols)   8 = BKSP   9 = OK

enum KbLayout { KB_ALPHA = 0, KB_SYMBOLS };
enum KbResult { KBR_NONE, KBR_CHANGED, KBR_DONE, KBR_CANCEL, KBR_LAYOUT, KBR_CAPS };

// ─────────────────────────────────────────────────────────────────────────────
// Geometry helpers — keyboard occupies the bottom half of the screen.
// ─────────────────────────────────────────────────────────────────────────────
static inline int16_t kbH(uint16_t sh)   { return (int16_t)(sh / 2); }
static inline int16_t kbY(uint16_t sh)   { return (int16_t)(sh - kbH(sh)); }
static inline int16_t cellW(uint16_t sw) { return (int16_t)(sw / KB_COLS); }
static inline int16_t cellH(uint16_t sh) { return (int16_t)(kbH(sh) / KB_ROWS); }

// ─────────────────────────────────────────────────────────────────────────────
// Touch polling (raw, no debounce — debounce handled below)
// ─────────────────────────────────────────────────────────────────────────────
static bool kb_rawTouch(TFT_eSPI& tft, uint16_t* x, uint16_t* y) {
#ifdef HAS_CAP_TOUCH
    if (!ft6336_update(x, y)) return false;
    // FT6336 reports panel-native (portrait) coords; map to the active rotation so
    // they match the keyboard's tft.width()/height() layout.
    uint8_t  rot = tft.getRotation() & 3;
    uint16_t W0  = (rot & 1) ? (uint16_t)tft.height() : (uint16_t)tft.width();
    uint16_t H0  = (rot & 1) ? (uint16_t)tft.width()  : (uint16_t)tft.height();
    uint16_t rx = *x, ry = *y;
    switch (rot) {
        case 0: *x = rx;                      *y = ry;                      break;
        case 1: *x = ry;                      *y = (uint16_t)(W0 - 1 - rx); break;
        case 2: *x = (uint16_t)(W0 - 1 - rx); *y = (uint16_t)(H0 - 1 - ry); break;
        case 3: *x = (uint16_t)(H0 - 1 - ry); *y = rx;                      break;
    }
    return true;
#else
    return tft.getTouch(x, y, KB_XPT_THRESHOLD);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing
// ─────────────────────────────────────────────────────────────────────────────
// Bounds of the SHOW/HIDE toggle (password mode only). Sits just below the
// input box, right-aligned. Geometry is deterministic from scrW + title so both
// the drawing code and the touch handler can compute it identically.
static void passToggleRect(uint16_t scrW, const char* title,
                           int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    int16_t titleH = (title && title[0]) ? 22 : 0;
    int16_t boxY = 6 + titleH;
    int16_t boxH = 26;
    w = 66;
    h = 22;
    x = (int16_t)scrW - 8 - w;
    y = boxY + boxH + 4;
}

static void drawTextArea(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, uint16_t scrH,
                         const char* title, const char* buffer,
                         bool password, bool reveal) {
    int16_t areaH = kbY(scrH);
    tft.fillRect(0, 0, (int16_t)scrW, areaH, bg);

    int16_t y = 6;
    if (title && title[0]) {
        tft.setTextColor(TFT_GREEN, bg);
        tft.drawString(title, 4, y, 2);
        y += 22;
    }

    // Framed input box
    int16_t boxY = y;
    int16_t boxH = 26;
    tft.drawRect(4, boxY, (int16_t)scrW - 8, boxH, fg);

    tft.setTextColor(fg, bg);
    tft.setTextDatum(TL_DATUM);
    int16_t tx = 8;
    int16_t ty = boxY + (boxH - 16) / 2;
    const char* p = buffer ? buffer : "";
    size_t n = strlen(p);
    // Mask only when it's a password and the user hasn't tapped SHOW.
    bool mask = password && !reveal;
    char shown[64];
    if (mask) {
        size_t k = n < sizeof(shown) - 1 ? n : sizeof(shown) - 1;
        memset(shown, '*', k);
        shown[k] = '\0';
    } else {
        strncpy(shown, p, sizeof(shown) - 1);
        shown[sizeof(shown) - 1] = '\0';
    }
    // Show the tail if the text overflows: trim from the left until it fits.
    int16_t maxW = (int16_t)scrW - 8 - 12;
    char* s = shown;
    while (tft.textWidth(s, 2) > maxW && *s) s++;
    tft.drawString(s, tx, ty, 2);
    // Cursor bar
    int16_t cx = tx + tft.textWidth(s, 2);
    if (cx > (int16_t)scrW - 8) cx = (int16_t)scrW - 8;
    tft.fillRect(cx + 1, ty, 2, 16, fg);

    // SHOW / HIDE toggle for password fields.
    if (password) {
        int16_t bx, by, bw, bh;
        passToggleRect(scrW, title, bx, by, bw, bh);
        uint16_t f = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x18E3;
        tft.fillRoundRect(bx, by, bw, bh, 4, f);
        tft.drawRoundRect(bx, by, bw, bh, 4, fg);
        tft.setTextColor(fg, f);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(reveal ? "HIDE" : "SHOW", bx + bw / 2, by + bh / 2, 2);
        tft.setTextDatum(TL_DATUM);
    }
}

static void drawKeyboard(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, uint16_t scrH,
                         KbLayout layout, bool caps) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    uint16_t key_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t key_fg = fg;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;

    tft.fillRect(0, kY, (int16_t)scrW, kH, key_bg);

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // Rows 0-3: character rows
    for (int r = 0; r < 4; r++) {
        const char* row    = rows[r];
        int         rowLen = (int)strlen(row);
        int16_t     rowY   = kY + (int16_t)r * cH;

        int16_t xOff;
        if (layout == KB_ALPHA && r == 3) xOff = 0;                        // left-aligned for CAPS
        else xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);                // centered

        for (int i = 0; i < rowLen; i++) {
            int16_t kx = (int16_t)i * cW + xOff;
            tft.drawRect(kx, rowY, cW, cH, bdr);
            char c = row[i];
            if (layout == KB_ALPHA && r >= 1 && c >= 'a' && c <= 'z' && caps)
                c = (char)(c - 'a' + 'A');
            int16_t tcy = rowY + (cH - 16) / 2;
            tft.setTextColor(key_fg, key_bg);
            char c_str[2] = { c, '\0' };
            tft.drawCentreString(c_str, kx + cW / 2, tcy, 2);
        }

        // CAPS key: cols 8-9 in alpha row 3
        if (layout == KB_ALPHA && r == 3) {
            int16_t cx  = 8 * cW;
            int16_t cw2 = 2 * cW;
            uint16_t caps_bg = caps ? (uint16_t)0x07E0 : key_bg;
            uint16_t caps_fg = caps ? (uint16_t)TFT_BLACK : key_fg;
            tft.fillRect(cx, rowY, cw2, cH, caps_bg);
            tft.drawRect(cx, rowY, cw2, cH, bdr);
            tft.setTextColor(caps_fg, caps_bg);
            tft.drawCentreString(caps ? "caps" : "CAPS", cx + cW, rowY + (cH - 16) / 2, 2);
        }
    }

    // Row 4: control row
    int16_t rowY  = kY + 4 * cH;
    int16_t ctrlY = rowY + (cH - 16) / 2;

    // Col 0: CANCEL
    tft.drawRect(0, rowY, cW, cH, bdr);
    tft.setTextColor(TFT_RED, key_bg);
    tft.drawCentreString("X", cW / 2, ctrlY, 2);

    // Col 1: SYM / ABC toggle
    tft.drawRect(cW, rowY, cW, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString(layout == KB_ALPHA ? "SYM" : "ABC", cW + cW / 2, ctrlY, 2);

    // Cols 2-7: SPACE
    tft.fillRect(2 * cW, rowY, 6 * cW, cH, key_bg);
    tft.drawRect(2 * cW, rowY, 6 * cW, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    tft.drawCentreString("SPACE", 5 * cW, ctrlY, 2);

    // Col 8: BKSP (left-arrow glyph)
    tft.drawRect(8 * cW, rowY, cW, cH, bdr);
    {
        int16_t ax = 8 * cW + cW / 2 - 5;
        int16_t ay = rowY + cH / 2;
        tft.fillRect(ax + 3, ay - 3, 1, 1, key_fg);
        tft.fillRect(ax + 2, ay - 2, 2, 1, key_fg);
        tft.fillRect(ax + 1, ay - 1, 3, 1, key_fg);
        tft.fillRect(ax + 0, ay + 0, 4, 1, key_fg);
        tft.fillRect(ax + 1, ay + 1, 3, 1, key_fg);
        tft.fillRect(ax + 2, ay + 2, 2, 1, key_fg);
        tft.fillRect(ax + 3, ay + 3, 1, 1, key_fg);
        tft.fillRect(ax + 4, ay + 0, 6, 1, key_fg);
    }

    // Col 9: OK
    tft.fillRect(9 * cW, rowY, cW, cH, (uint16_t)0x07E0);
    tft.drawRect(9 * cW, rowY, cW, cH, bdr);
    tft.setTextColor((uint16_t)TFT_BLACK, (uint16_t)0x07E0);
    tft.drawCentreString("OK", 9 * cW + cW / 2, ctrlY, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Buffer helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool appendByte(char* buf, size_t bufLen, char c) {
    size_t len = strlen(buf);
    if (len + 1 < bufLen) { buf[len] = c; buf[len + 1] = '\0'; return true; }
    return false;
}

static KbResult handleKbTouch(uint16_t tx, uint16_t ty,
                              uint16_t scrW, uint16_t scrH,
                              char* buffer, size_t bufLen,
                              KbLayout layout, bool caps) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    if ((int16_t)ty < kY || (int16_t)ty >= kY + kH) return KBR_NONE;
    int row = ((int16_t)ty - kY) / cH;
    if (row < 0 || row >= KB_ROWS) return KBR_NONE;

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    if (row <= 3) {
        const char* rowStr = rows[row];
        int rowLen = (int)strlen(rowStr);

        if (layout == KB_ALPHA && row == 3) {
            if ((int16_t)tx >= 8 * cW) return KBR_CAPS;          // CAPS cols 8-9
            int col = (int16_t)tx / cW;
            if (col < 0 || col >= rowLen) return KBR_NONE;
            char c = rowStr[col];
            if (c >= 'a' && c <= 'z' && caps) c = (char)(c - 'a' + 'A');
            return appendByte(buffer, bufLen, c) ? KBR_CHANGED : KBR_NONE;
        }

        int16_t xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);
        int col = ((int16_t)tx - xOff) / cW;
        if (col < 0 || col >= rowLen) return KBR_NONE;
        char c = rowStr[col];
        if (layout == KB_ALPHA && row >= 1 && c >= 'a' && c <= 'z' && caps)
            c = (char)(c - 'a' + 'A');
        return appendByte(buffer, bufLen, c) ? KBR_CHANGED : KBR_NONE;
    }

    // Control row 4
    int col = (int16_t)tx / cW;
    switch (col) {
        case 0: return KBR_CANCEL;
        case 1: return KBR_LAYOUT;
        case 2: case 3: case 4: case 5: case 6: case 7:
            return appendByte(buffer, bufLen, ' ') ? KBR_CHANGED : KBR_NONE;
        case 8: {
            size_t len = strlen(buffer);
            if (len > 0) { buffer[len - 1] = '\0'; return KBR_CHANGED; }
            return KBR_NONE;
        }
        case 9: return KBR_DONE;
        default: return KBR_NONE;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
bool touchKeyboardInput(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                        char* buffer, size_t bufLen,
                        const char* title, bool password) {
    if (!buffer || bufLen < 2) return false;

    uint16_t scrW = (uint16_t)tft.width();
    uint16_t scrH = (uint16_t)tft.height();
    KbLayout layout = KB_ALPHA;
    bool     caps   = false;
    bool     reveal = false;

    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, password, reveal);
    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);

    uint32_t lastTouch = 0;
    const uint32_t debounce = 150;

    for (;;) {
        uint16_t tx = 0, ty = 0;
        if (kb_rawTouch(tft, &tx, &ty)) {
            uint32_t now = millis();
            if (now - lastTouch < debounce) { delay(5); continue; }
            lastTouch = now;

            // SHOW / HIDE toggle (password fields) — handled before the keys.
            if (password) {
                int16_t bx, by, bw, bh;
                passToggleRect(scrW, title, bx, by, bw, bh);
                if ((int16_t)tx >= bx && (int16_t)tx < bx + bw &&
                    (int16_t)ty >= by && (int16_t)ty < by + bh) {
                    reveal = !reveal;
                    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, password, reveal);
                    continue;
                }
            }

            KbResult r = handleKbTouch(tx, ty, scrW, scrH, buffer, bufLen, layout, caps);
            switch (r) {
                case KBR_CHANGED:
                    drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, password, reveal);
                    break;
                case KBR_DONE:
                    return true;
                case KBR_CANCEL:
                    return false;
                case KBR_LAYOUT:
                    layout = (layout == KB_ALPHA) ? KB_SYMBOLS : KB_ALPHA;
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);
                    break;
                case KBR_CAPS:
                    caps = !caps;
                    drawKeyboard(tft, fg, bg, scrW, scrH, layout, caps);
                    break;
                default: break;
            }
        }
        delay(5);
        yield();
    }
}

#endif // HAS_TOUCH
