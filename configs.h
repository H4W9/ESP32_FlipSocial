#pragma once
#ifndef configs_h
#define configs_h

// Board select.
// CI passes -DMARAUDER_PANCAKE / -DMARAUDER_V8 on the compile line; for a local
// Arduino IDE build uncomment exactly one below. Whichever you pick, point
// libraries/TFT_eSPI-ESP32-C5/User_Setup_Select.h at the matching board setup
// (User_Setup_marauder_pancake.h / User_Setup_marauder_v8.h).
#if !defined(MARAUDER_PANCAKE) && !defined(MARAUDER_V8)
  #define MARAUDER_PANCAKE
  // #define MARAUDER_V8
#endif

// Firmware identity — shown on the Settings → About screen.
// Bump FW_VERSION when cutting a release. FW_COMMIT is stamped by CI at build
// time (see .github/workflows/build.yml); it stays "dev" for local builds.
#define FW_NAME    "FlipSocial"
#define FW_AUTHOR  "H4W9"
#define FW_VERSION "1.0.1"
#ifndef FW_COMMIT
#define FW_COMMIT  "dev"
#endif

// Uncomment for verbose serial logging.
// #define DEVELOPER

// MARAUDER_PANCAKE
//   ESP32-C5, shared FSPI bus (TFT + SD), FT6336 capacitive touch via I2C, PSRAM.
#ifdef MARAUDER_PANCAKE
  #define BOARD_NAME    "Pancake C5"
  #define BOARD_MCU     "ESP32-C5"
  #define BOARD_DISPLAY "ST7796 320x480"
  #define BOARD_TOUCH   "FT6336 capacitive"

  #define HAS_SCREEN
  #define HAS_FULL_SCREEN
  #define HAS_TOUCH
  #define HAS_CAP_TOUCH       // FT6336 capacitive — no calibration needed
  #define HAS_SD
  #define USE_SD
  #define HAS_C5_SD           // explicit SPIClass init required before SD.begin()
  #define HAS_PSRAM
  #define HAS_IDF_3           // ESP32-C5 uses IDF 5.x; psramInit() handles PSRAM
  #define HAS_BATTERY         // MAX17048 fuel gauge on shared I2C bus

  #define TFT_WIDTH  320
  #define TFT_HEIGHT 480

  #define SD_CS   7           // SD chip-select

  #define I2C_SDA  9
  #define I2C_SCL 10
  // FT6336 capacitive touch controller (shares I2C bus)
  #define CTP_RST  8
  #define CTP_SDA  I2C_SDA
  #define CTP_SCL  I2C_SCL
#endif

// MARAUDER_V8
//   ESP32-C5, shared FSPI bus (TFT + SD + XPT2046), PSRAM.
//   Touch is resistive: it needs calibration, stored in NVS (see touchCalLoad).
#ifdef MARAUDER_V8
  #define BOARD_NAME    "Marauder V8"
  #define BOARD_MCU     "ESP32-C5"
  #define BOARD_DISPLAY "ILI9341 240x320"
  #define BOARD_TOUCH   "XPT2046 resistive"

  #define HAS_SCREEN
  #define HAS_FULL_SCREEN
  #define HAS_TOUCH           // XPT2046 resistive — no HAS_CAP_TOUCH
  #define HAS_SD
  #define USE_SD
  #define HAS_C5_SD           // explicit SPIClass init required before SD.begin()
  #define HAS_PSRAM
  #define HAS_IDF_3           // ESP32-C5 uses IDF 5.x; psramInit() handles PSRAM
  #define HAS_BATTERY         // MAX17048 fuel gauge on shared I2C bus

  #define TFT_WIDTH  240
  #define TFT_HEIGHT 320

  #define SD_CS  10           // SD chip-select

  #define I2C_SCL  4
  #define I2C_SDA  5
#endif

#define SCREEN_WIDTH  TFT_WIDTH
#define SCREEN_HEIGHT TFT_HEIGHT

// Shared FSPI bus — the SPI pins come from the TFT_eSPI User_Setup for the board.
#define SD_MISO TFT_MISO
#define SD_MOSI TFT_MOSI
#define SD_SCK  TFT_SCLK

#endif // configs_h
