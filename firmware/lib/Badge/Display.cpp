#include "Display.h"

#include <stdarg.h>
#include <stdio.h>

namespace badge {
namespace {
constexpr int16_t kHeaderH  = 26;
constexpr int16_t kRowH     = 20;
constexpr int16_t kPadX     = 6;
constexpr uint8_t kTextSize = 2;  // 6x8 base font * 2 = 12x16 px glyphs
}  // namespace

void Display::begin() {
  // Custom SPI pins — this board uses none of the usual defaults, so the bus
  // is brought up explicitly before the panel driver touches it. MISO is -1:
  // the panel is write-only as wired.
  spi_.begin(PIN_LCD_CLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_LCD_CS);

  // Native panel orientation is 240x320 portrait; rotation makes it landscape.
  tft_.init(240, 320);
  tft_.setSPISpeed(LCD_SPI_HZ);

  // §4: invert_color(true) is required on this panel, or every colour comes
  // out as a photographic negative.
  tft_.invertDisplay(true);

  // swap_xy + mirror from §4, expressed as an Adafruit_GFX rotation.
  tft_.setRotation(1);

  ready_ = true;
  clear();
}

void Display::clear(uint16_t color) {
  if (!ready_) return;
  tft_.fillScreen(color);
}

void Display::header(const char* text) {
  if (!ready_ || text == nullptr) return;
  tft_.fillRect(0, 0, LCD_W, kHeaderH, ST77XX_BLUE);
  tft_.setTextColor(ST77XX_WHITE);
  tft_.setTextSize(kTextSize);
  tft_.setCursor(kPadX, 5);
  tft_.print(text);
}

int16_t Display::rowY(uint8_t row) const {
  return static_cast<int16_t>(kHeaderH + 4 + row * kRowH);
}

void Display::line(uint8_t row, const char* text, uint16_t color) {
  if (!ready_ || text == nullptr || row >= kRows) return;
  const int16_t y = rowY(row);

  // Clear only this row. Cheaper than a repaint, and it avoids the full-frame
  // buffer the memory rule in the header forbids.
  tft_.fillRect(0, y, LCD_W, kRowH, ST77XX_BLACK);

  tft_.setTextColor(color);
  tft_.setTextSize(kTextSize);
  tft_.setCursor(kPadX, y + 2);
  tft_.print(text);
}

void Display::linef(uint8_t row, uint16_t color, const char* fmt, ...) {
  char buf[48];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  line(row, buf, color);
}

void Display::setRotationVariant(uint8_t rotation) {
  if (!ready_) return;
  tft_.setRotation(rotation);
  clear();
}

}  // namespace badge
