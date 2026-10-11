#include "button_logic.h"

namespace badge {
namespace {

// Indexed by Button. Kept lowercase so console parsing is case-flexible.
const char* const kNames[BTN_COUNT] = {
    "a", "b", "home", "down", "left", "right", "up", "aux1", "start",
};

bool sameStringCaseless(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != '\0' && *b != '\0') {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

}  // namespace

const char* buttonName(uint8_t button) {
  return button < BTN_COUNT ? kNames[button] : "?";
}

int buttonFromName(const char* name) {
  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    if (sameStringCaseless(name, kNames[i])) return static_cast<int>(i);
  }
  return -1;
}

uint16_t decodeShiftSamples(const uint8_t* samples) {
  if (samples == nullptr) return 0;
  uint16_t mask = 0;
  for (uint8_t i = 0; i < kShiftCount; ++i) {
    if (samples[i] == 0) mask |= static_cast<uint16_t>(1u << i);  // active-low
  }
  return mask;
}

uint16_t decodeShiftByte(uint8_t packed) {
  // Invert (active-low) and keep only the 8 shift-register bits.
  return static_cast<uint16_t>(static_cast<uint8_t>(~packed));
}

Debouncer::Debouncer(uint8_t stableSamples)
    : needed_(stableSamples == 0 ? 1 : stableSamples) {}

void Debouncer::prime(uint16_t raw) {
  stable_   = raw;
  pressed_  = 0;
  released_ = 0;
  for (uint8_t i = 0; i < BTN_COUNT; ++i) counts_[i] = 0;
}

uint16_t Debouncer::update(uint16_t raw) {
  pressed_  = 0;
  released_ = 0;

  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    const uint16_t bit     = static_cast<uint16_t>(1u << i);
    const bool rawDown     = (raw & bit) != 0;
    const bool stableDown  = (stable_ & bit) != 0;

    if (rawDown == stableDown) {
      counts_[i] = 0;  // agrees with accepted state; nothing pending
      continue;
    }

    if (++counts_[i] >= needed_) {
      counts_[i] = 0;
      if (rawDown) {
        stable_  |= bit;
        pressed_ |= bit;
      } else {
        stable_   = static_cast<uint16_t>(stable_ & ~bit);
        released_ |= bit;
      }
    }
  }

  return stable_;
}

bool Debouncer::isDown(uint8_t button) const {
  return button < BTN_COUNT && (stable_ & (1u << button)) != 0;
}

bool Debouncer::wasPressed(uint8_t button) const {
  return button < BTN_COUNT && (pressed_ & (1u << button)) != 0;
}

bool Debouncer::wasReleased(uint8_t button) const {
  return button < BTN_COUNT && (released_ & (1u << button)) != 0;
}

}  // namespace badge
