#include "Buttons.h"

#include "badge_pins.h"

namespace badge {

void Buttons::begin() {
  pinMode(PIN_HC165_DATA, INPUT);
  pinMode(PIN_HC165_LOAD, OUTPUT);
  pinMode(PIN_HC165_CLK, OUTPUT);

  // Idle state: LOAD high (not latching), CLK low (ready for a rising edge).
  digitalWrite(PIN_HC165_LOAD, HIGH);
  digitalWrite(PIN_HC165_CLK, LOW);

  // Start is active-low with its own GPIO. INPUT_PULLUP is harmless here and
  // keeps the line defined if the pushbutton is ever left floating.
  pinMode(PIN_START, INPUT_PULLUP);

  deb_.prime(readRaw());
  lastPollMs_ = millis();
}

uint8_t Buttons::readShiftByte() {
  // §3 read protocol:
  //   1. pull LOAD low then high  -> latches all 8 inputs
  //   2. 8x: sample DATA, then pulse CLK high-low
  // ~1 us delays are explicitly fine, so this stays a simple bit-bang.
  digitalWrite(PIN_HC165_LOAD, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN_HC165_LOAD, HIGH);
  delayMicroseconds(1);

  uint8_t packed = 0;
  for (uint8_t i = 0; i < kShiftCount; ++i) {
    // Sample BEFORE clocking: the latched bit is already presented on DATA.
    if (digitalRead(PIN_HC165_DATA)) packed |= static_cast<uint8_t>(1u << i);

    digitalWrite(PIN_HC165_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_HC165_CLK, LOW);
    delayMicroseconds(1);
  }
  return packed;
}

uint16_t Buttons::readRaw() {
  lastRawByte_ = readShiftByte();
  uint16_t mask = decodeShiftByte(lastRawByte_);

  if (digitalRead(PIN_START) == LOW) mask |= static_cast<uint16_t>(1u << BTN_START);

  // Fold in anything the console asked us to fake.
  if (injectedMask_ != 0) {
    if (static_cast<int32_t>(millis() - injectUntilMs_) >= 0) {
      injectedMask_ = 0;
    } else {
      mask |= injectedMask_;
    }
  }
  return mask;
}

bool Buttons::poll() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastPollMs_) < 10) return false;  // §3: ~10 ms
  lastPollMs_ = now;

  deb_.update(readRaw());
  return true;
}

void Buttons::inject(uint8_t button, uint32_t ms) {
  if (button >= BTN_COUNT) return;
  injectedMask_ |= static_cast<uint16_t>(1u << button);
  injectUntilMs_ = millis() + ms;
}

}  // namespace badge
