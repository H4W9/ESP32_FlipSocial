# Pancake Picoware

A standalone HTTP firmware for the **Pancake** board (ESP32-C5, ST7796 320×480,
FT6336 capacitive touch) — a touch port of [Picoware](https://github.com/jblanked/Picoware)
that natively runs **FlipSocial** and **FlipWorld** with their own WiFi/HTTP, no
Flipper Zero attached.

## Status — staged build

| Milestone | Scope | State |
|-----------|-------|-------|
| **M1** | Boot + hardware bring-up, touch main menu, WiFi scan/connect (touch keyboard), test HTTPS GET | **code complete — needs a compile pass on hardware** |
| M2 | FlipSocial on the touch shell | not started |
| M3 | FlipWorld with on-screen d-pad | not started |

## Build (Arduino IDE)

| Setting | Value |
|---------|-------|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| Partition Scheme | Custom → `partitions.csv` in this folder |
| Flash Frequency | 80 MHz |

### Required library
Install the **patched `TFT_eSPI-ESP32-C5`** used by the Bible firmware
(`../bible_firmware/libraries/TFT_eSPI-ESP32-C5`) into your Arduino `libraries`
folder, and set `User_Setup_Select.h` to:

```cpp
#include <User_Setup_marauder_pancake.h>
```

Also required (all already common / installable via Library Manager):
`ArduinoJson`, `ArduinoHttpClient`. The ESP32-C5 Arduino core provides the rest.

An SD card is used for Picoware settings/state files (WiFi credentials are stored
in NVS via `Preferences`). FAT32.

## What M1 does

1. Brings up backlight (PWM), SD (shared FSPI), PSRAM, FT6336 touch, and the
   ST7796 panel (through Picoware's `Draw`).
2. Shows a touch main menu: **WiFi Setup**, FlipSocial (M2), FlipWorld (M3).
3. WiFi Setup: scans, lists networks (tap to pick), enter password on the touch
   keyboard, connects, saves credentials, then runs a test HTTPS GET through
   Picoware's `HTTP` class and prints the response.

## Layout

```
pancake_picoware.ino     M1 sketch: bring-up + touch shell + WiFi + HTTP test
configs.h                Pancake pin/board config (mirrors the Bible firmware)
ft6336.h                 FT6336 capacitive-touch driver (from the Bible firmware)
TouchKeyboard.{h,cpp}    Self-contained touch QWERTY keyboard (modeled on ESP32_Bible)
partitions.csv           8 MB layout (nvs + ota_0 app + fat/spiffs)
src/Picoware/            Vendored Picoware core, ported to ESP32-C5:
  internal/boards.hpp        + BOARD_TYPE_PANCAKE / PancakeConfig
  internal/gui/draw.*        PicoDVI guarded out; TFT_eSPI backend; getTFT() accessor
  internal/system/input.*    Pico peripherals removed; FT6336 TouchInput added
  internal/system/input_manager.hpp   Pancake touch branch + getTouch()
  internal/system/storage.hpp         SD-backed (was Pico LittleFS)
  internal/system/system.hpp          ESP.* heap/reboot (was rp2040.*)
  internal/system/http.hpp            setInsecure() so HTTPS works
  internal/system/{wifi_utils,wifi_ap,keyboard,view_manager,...}  (ESP32-native as-is)
```

### Hybrid input model
`TouchInput` (FT6336) both maps screen tap-zones to Picoware's `BUTTON_*` codes —
so Picoware's existing button-navigated views and games work unchanged — and
exposes a raw touch point for direct-tap menus. FlipWorld (M3) will get an
on-screen d-pad overlay driving those button codes.
