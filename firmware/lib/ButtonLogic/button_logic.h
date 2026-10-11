// button_logic.h — button decoding and debouncing, with ZERO Arduino includes.
//
// This file is deliberately portable: it compiles and runs on your laptop under
// `pio test -e native`. All the logic that is easy to get subtly wrong — shift
// order, active-low inversion, debounce timing, the Aux1 boot state — lives
// here where a test can prove it, instead of inside a GPIO routine where the
// only way to check it is to flash a board and press things with your thumb.
//
// If you ever need to #include <Arduino.h> in here, the logic belongs in
// lib/Badge instead.

#pragma once

#include <stdint.h>

namespace badge {

// Order matters: indices 0..7 are the 74HC165 shift order from HAL guide §3,
// "A shifts out first, Aux1 last". Start is index 8 because it is wired to its
// own GPIO and is not on the shift register at all.
enum Button : uint8_t {
  BTN_A = 0,
  BTN_B,
  BTN_HOME,
  BTN_DOWN,
  BTN_LEFT,
  BTN_RIGHT,
  BTN_UP,
  BTN_AUX1,
  BTN_START,
  BTN_COUNT
};

constexpr uint8_t kShiftCount = 8;  // buttons on the 74HC165

// Human-readable name, e.g. "a", "up", "aux1", "start". Used by the serial
// console so you can type `btn up` instead of remembering bit numbers.
const char* buttonName(uint8_t button);

// Parse a name back to a Button. Returns -1 if unrecognised.
int buttonFromName(const char* name);

// Decode 8 raw DATA samples into a pressed-bitmask.
// samples[i] is the level read at shift position i. Buttons are ACTIVE-LOW
// (§3: "a sampled 0 = pressed"), so this inverts for you — the single most
// common place to introduce an off-by-inverted bug.
uint16_t decodeShiftSamples(const uint8_t* samples);

// Same, but from a packed byte where bit i holds the sample for shift
// position i. Convenience for tests and for a bit-banged read that already
// accumulated into a byte.
uint16_t decodeShiftByte(uint8_t packed);

// Per-button debouncer.
//
// A button must read the same new value for `stableSamples` consecutive
// updates before the change is accepted. At the §3-recommended 10 ms poll,
// the default of 2 gives ~20 ms of debounce, which is comfortably past
// contact bounce without feeling laggy.
class Debouncer {
 public:
  explicit Debouncer(uint8_t stableSamples = 2);

  // Feed a raw pressed-mask. Returns the debounced mask.
  uint16_t update(uint16_t raw);

  // Adopt `raw` as the current state WITHOUT generating press/release edges.
  //
  // This exists for one specific reason: HAL guide §3 warns that Aux1 is a
  // maintained side switch, not a momentary button, so it may already be held
  // down at boot. Without priming, the first update() would report a fresh
  // "Aux1 pressed" edge that the user never performed, and any app that starts
  // a mode on that edge would start in the wrong mode. Call this once after
  // your first hardware read.
  void prime(uint16_t raw);

  uint16_t stable() const { return stable_; }
  uint16_t pressedEdges() const { return pressed_; }    // went down this update
  uint16_t releasedEdges() const { return released_; }  // came up this update

  bool isDown(uint8_t button) const;
  bool wasPressed(uint8_t button) const;
  bool wasReleased(uint8_t button) const;

 private:
  uint8_t needed_;
  uint8_t counts_[BTN_COUNT] = {0};
  uint16_t stable_           = 0;
  uint16_t pressed_          = 0;
  uint16_t released_         = 0;
};

}  // namespace badge
