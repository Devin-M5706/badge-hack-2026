// Buttons.h — 74HC165 shift register + dedicated Start button (HAL guide §3).
//
// The GPIO half of button handling. All decode/debounce logic lives in
// lib/ButtonLogic so it can be tested on a laptop; this class only moves pins
// and hands raw samples over.

#pragma once

#include <Arduino.h>

#include "button_logic.h"

namespace badge {

class Buttons {
 public:
  // Configures pins and primes the debouncer from a first real read.
  //
  // Priming matters: §3 warns Aux1 is a maintained side switch, so it may
  // already be held at boot. Priming adopts that as the starting state instead
  // of reporting a phantom press on the first poll.
  void begin();

  // Call as often as you like. Internally rate-limited to the §3-recommended
  // 10 ms. Returns true when a fresh sample was actually taken, which is also
  // when edge queries below are meaningful.
  bool poll();

  bool isDown(uint8_t button) const { return deb_.isDown(button); }
  bool wasPressed(uint8_t button) const { return deb_.wasPressed(button); }
  bool wasReleased(uint8_t button) const { return deb_.wasReleased(button); }

  uint16_t stable() const { return deb_.stable(); }
  uint16_t pressedEdges() const { return deb_.pressedEdges(); }
  uint16_t releasedEdges() const { return deb_.releasedEdges(); }

  // Last packed byte read off the shift register, pre-inversion. Handy when a
  // button seems dead and you want to see whether the wire is even moving.
  uint8_t lastRawByte() const { return lastRawByte_; }

  // Synthesise a button hold for `ms`, as if it were physically pressed.
  // This is what makes the serial console (§9) useful: you can drive app logic
  // over USB without reaching for the board.
  void inject(uint8_t button, uint32_t ms = 60);

 private:
  uint8_t readShiftByte();
  uint16_t readRaw();

  Debouncer deb_{2};  // 2 samples @ 10 ms = ~20 ms debounce
  uint32_t lastPollMs_    = 0;
  uint8_t lastRawByte_    = 0xFF;
  uint16_t injectedMask_  = 0;
  uint32_t injectUntilMs_ = 0;
};

}  // namespace badge
