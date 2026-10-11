// Console.h — line-based debug console over USB-Serial-JTAG (HAL guide §9).
//
// §9: "keep at least a button-inject + print path over USB-Serial-JTAG — being
// able to drive buttons and read state over USB is what makes HAL development
// fast." That is the whole reason this exists. With it you can exercise app
// logic from your laptop keyboard instead of holding a 2 cm button down with a
// fingernail while squinting at a 320x240 panel.
//
// Type `help` on the serial monitor for the command list.

#pragma once

#include <Arduino.h>

namespace badge {

class Accel;
class Buttons;
class Display;
class Leds;

class Console {
 public:
  // Any of these may be null — commands that need a missing subsystem just
  // report that it is unavailable, so a half-built badge is still debuggable.
  void begin(Buttons* buttons, Leds* leds, Accel* accel, Display* display);

  // Non-blocking; call every loop iteration.
  void poll();

 private:
  void dispatch(char* line);
  void printHelp();
  void printState();

  // §9: keep each USB write under 256 bytes (USB-Serial-JTAG RX ring). Longer
  // output is split across several writes rather than one big one.
  void writeChunked(const char* text);

  Buttons* buttons_ = nullptr;
  Leds*    leds_    = nullptr;
  Accel*   accel_   = nullptr;
  Display* display_ = nullptr;

  char   buf_[80] = {0};
  size_t len_     = 0;
};

}  // namespace badge
