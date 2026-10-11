// Display.h — ST7789 320x240 over SPI2 (HAL guide §4).
//
// IMPORTANT MEMORY RULE: there is no PSRAM on this module and ~400 KB of SRAM
// total. A full-frame RGB565 buffer is 320*240*2 = 150 KB, and §4 says plainly
// "do not allocate full-frame DMA buffers". So this class never creates a
// GFXcanvas16 — it draws straight to the panel, and clears only the rectangle
// it is about to redraw. If you are tempted to add an off-screen canvas for
// flicker-free compositing, clear a row instead.

#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>

#include "badge_pins.h"

namespace badge {

class Display {
 public:
  void begin();

  void clear(uint16_t color = ST77XX_BLACK);

  // Title bar across the top.
  void header(const char* text);

  // Write one text row (0-based, below the header). Clears just that row's
  // rectangle first, so repeated updates do not smear or need a full repaint.
  void line(uint8_t row, const char* text, uint16_t color = ST77XX_WHITE);

  // printf-style convenience for the same.
  void linef(uint8_t row, uint16_t color, const char* fmt, ...);

  // If the image comes up rotated or mirrored, §4 says the fix is the
  // invert/swap_xy/mirror trio. Under Adafruit_GFX that collapses to rotation:
  // try 1 (the default here) and 3. Exposed so you can try the other one
  // without touching anything else.
  void setRotationVariant(uint8_t rotation);

  Adafruit_ST7789& raw() { return tft_; }

  static constexpr uint8_t kRows = 9;

 private:
  int16_t rowY(uint8_t row) const;

  // The panel driver is constructed with its pins up front rather than being
  // reassigned in begin(), so there is exactly one place the wiring is stated.
  SPIClass spi_{FSPI};  // SPI2 host on the ESP32-C3
  Adafruit_ST7789 tft_{&spi_, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST};
  bool ready_ = false;
};

}  // namespace badge
