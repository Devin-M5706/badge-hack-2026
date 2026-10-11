// Leds.h — 6x WS2812B-2020 on a single data line (HAL guide §6).
//
// Brightness is capped on purpose. §6: "6 LEDs at full white can brown-out the
// board on AA power." A brown-out mid-demo looks exactly like a crash, and you
// will go hunting through your code for a bug that is actually a power budget.

#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "badge_pins.h"

namespace badge {

// Physical positions looking at the front of the badge (§6). Named so a chase
// animation reads as a direction instead of as index arithmetic.
enum LedPos : uint8_t {
  LED_UPPER_LEFT   = 0,
  LED_UPPER_RIGHT  = 1,
  LED_MIDDLE_RIGHT = 2,
  LED_BOTTOM_RIGHT = 3,
  LED_BOTTOM_LEFT  = 4,
  LED_MIDDLE_LEFT  = 5,
};

class Leds {
 public:
  void begin();

  void set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
  void setAll(uint8_t r, uint8_t g, uint8_t b);
  void clear();
  void show();

  // Raise the cap. Only sensible on USB power, and only if you know why.
  void setBrightnessUnsafe(uint8_t brightness);

  // Single dim LED walking around the board. Useful as a liveness indicator:
  // if the chase stops, your loop is blocked.
  void chase(uint32_t nowMs, uint16_t periodMs = 900);

 private:
  Adafruit_NeoPixel strip_{LED_COUNT, 0, NEO_GRB + NEO_KHZ800};
  uint8_t lastChaseIndex_ = 0xFF;
};

}  // namespace badge
