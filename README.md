# ESP32 FlipSocial (Pancake)

A standalone touch firmware that runs **FlipSocial** natively on the **Pancake**
board (ESP32-C5, ST7796 320×480, FT6336 capacitive touch) — no Flipper Zero
attached. It talks to the FlipSocial API directly over WiFi/HTTPS and is built on
[Picoware](https://github.com/jblanked/Picoware)'s panel / touch / HTTP core.

## Features

- **Feed** — smooth-scrolling chat-style bubbles; tap a post to Flip/Unflip, view
  or add comments; Prev/Next paging.
- **New Post** — compose and post from the touch keyboard.
- **Messages** — direct-message conversations; open a thread, send, page Prev/Next.
- **Explore** — search users, then view profile / add friend / message.
- **Profile** — your bio, friends count and join date; edit bio, manage friends,
  see your posts, change username/password, log out.
- **Settings** — 20 named themes, per-theme accent + font colour (with a Neon
  rainbow theme), screen brightness, RGB status-LED brightness, WiFi setup, and an
  About screen.
- **Touch keyboard** — QWERTY with tap-to-position cursor editing, shift-once vs.
  caps-lock, and a live character counter.
- **WiFi** — connects to saved networks in the background at boot; the header WiFi
  icon and the RGB status LED show connection/activity state.

## Build (Arduino IDE)

| Setting | Value |
|---------|-------|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| Partition Scheme | Custom → `partitions.csv` in this folder |
| Flash Frequency | 80 MHz |

### Libraries

- **`TFT_eSPI-ESP32-C5`** (the ESP32-C5-patched fork) — install into your Arduino
  `libraries` folder and set its `User_Setup_Select.h` to:

  ```cpp
  #include <User_Setup_marauder_pancake.h>
  ```
- **ArduinoJson** and **ArduinoHttpClient** — via Library Manager.

The ESP32-C5 Arduino core provides WiFi, HTTPS, SPIFFS and SD.

### Storage

- **SPIFFS** — UI settings (`/pico_ui.dat`), FlipSocial credentials
  (`/pico_user.json`) and saved WiFi networks (`/pico_wifi.json`).
- **SD (FAT32)** — used by the Picoware core for its own files.

## First run

1. Flash, then open **Settings → WiFi Setup → Scan**, pick your network and enter
   the password. It's saved and auto-connects on later boots.
2. In **Settings**, set your FlipSocial **Username** and **Password** (create an
   account first at [jblanked.com](https://www.jblanked.com/) if you don't have one).
3. Open **Feed** and you're in.

## Layout

```
ESP32_FlipSocial.ino   Main sketch: UI shell + all FlipSocial features
configs.h              Pancake pin / board config
theme.h                Themes, accents, font colours, brightness (SPIFFS)
ft6336.h               FT6336 capacitive-touch driver
TouchKeyboard.{h,cpp}  Self-contained touch QWERTY keyboard
partitions.csv         8 MB layout (nvs + ota apps + spiffs + fat)
src/Picoware/          Vendored Picoware core, ported to the ESP32-C5 Pancake
```

## Credits

- **[JBlanked](https://www.jblanked.com/)** — the **FlipSocial** app and API, and
  **[Picoware](https://github.com/jblanked/Picoware)**, which this firmware is built on.
