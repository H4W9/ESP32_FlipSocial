#pragma once
#ifndef configs_h
#define configs_h

// ─────────────────────────────────────────────────────────────────────────────
// Pancake Picoware — hardware config
//   Board: ESP32-C5, ST7796 320×480, FT6336 capacitive touch (I2C), PSRAM, SD.
//   This mirrors the MARAUDER_PANCAKE block from the Bible firmware so the shared
//   assets (ft6336.h, touch keyboard) compile unchanged.
// ─────────────────────────────────────────────────────────────────────────────

#define MARAUDER_PANCAKE

// Uncomment for verbose serial logging.
// #define DEVELOPER

#define HAS_SCREEN
#define HAS_FULL_SCREEN
#define HAS_TOUCH
#define HAS_CAP_TOUCH       // FT6336 capacitive — no calibration needed
#define HAS_SD
#define USE_SD
#define HAS_C5_SD           // explicit SPIClass init required before SD.begin()
#define HAS_PSRAM
#define HAS_IDF_3           // ESP32-C5 uses IDF 5.x; psramInit() handles PSRAM

#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define SCREEN_WIDTH  TFT_WIDTH
#define SCREEN_HEIGHT TFT_HEIGHT

#define SD_CS   7           // SD chip-select
#define SD_MISO TFT_MISO    // shared FSPI bus (defined in TFT_eSPI User_Setup)
#define SD_MOSI TFT_MOSI
#define SD_SCK  TFT_SCLK

#define I2C_SDA  9
#define I2C_SCL 10
// FT6336 capacitive touch controller (shares I2C bus)
#define CTP_RST  8
#define CTP_SDA  I2C_SDA
#define CTP_SCL  I2C_SCL

#define HAS_BATTERY         // MAX17048 fuel gauge on shared I2C bus

#endif // configs_h
