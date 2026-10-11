#include "Leds.h"

#include "badge_pins.h"

namespace badge {

void Leds::begin() {
  // Pin is set here rather than in the initialiser so badge_pins.h stays the
  // only place a GPIO number appears.
  strip_.setPin(PIN_LED_DIN);
  strip_.begin();
  strip_.setBrightness(LED_MAX_BRIGHTNESS);
  strip_.clear();
  strip_.show();
}

void Leds::set(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= LED_COUNT) return;
  strip_.setPixelColor(index, strip_.Color(r, g, b));
}

void Leds::setAll(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < LED_COUNT; ++i) set(i, r, g, b);
}

void Leds::clear() { strip_.clear(); }

void Leds::show() { strip_.show(); }

void Leds::setBrightnessUnsafe(uint8_t brightness) {
  strip_.setBrightness(brightness);
}

void Leds::chase(uint32_t nowMs, uint16_t periodMs) {
  if (periodMs == 0) return;
  const uint8_t index =
      static_cast<uint8_t>((nowMs / (periodMs / LED_COUNT + 1)) % LED_COUNT);
  if (index == lastChaseIndex_) return;  // only redraw on change
  lastChaseIndex_ = index;

  strip_.clear();
  set(index, 0, 0, 255);
  strip_.show();
}

}  // namespace badge
