// main.cpp — badge bring-up and liveness sketch.
//
// This is the "is my hardware alive" program, and it walks the minimal HAL
// checklist from HAL guide §10: Start button, HC165 read, 10 ms debounced
// poll, ST7789 init and fill, I2C accel WHO_AM_I and mg read, WS2812 chase.
// Flash this first. Once every line on the panel looks right, start replacing
// the body of loop() with your actual project.
//
// Loop discipline: the ESP32-C3 is single-core (§0 — "no second core to park
// work on; long compute blocks the UI loop"). Nothing here blocks. If you add
// a long computation, slice it across iterations or the LED chase will stutter
// and the console will stop answering, which is your early warning.

#include <Arduino.h>
#include <stdio.h>

#include "Accel.h"
#include "Buttons.h"
#include "Console.h"
#include "Display.h"
#include "Leds.h"
#include "badge_pins.h"

namespace {

badge::Display display;
badge::Buttons buttons;
badge::Leds leds;
badge::Accel accel;
badge::Console console;

// Screen rows, named so the layout is editable without counting.
constexpr uint8_t kRowAccelStatus = 0;
constexpr uint8_t kRowAccelValues = 1;
constexpr uint8_t kRowButtons     = 2;
constexpr uint8_t kRowLastEdge    = 3;
constexpr uint8_t kRowUptime      = 4;
constexpr uint8_t kRowHint        = 6;

uint32_t lastAccelMs = 0;
uint32_t lastClockMs = 0;

void renderPressedButtons() {
  char names[40];
  size_t used = 0;
  names[0]    = '\0';

  for (uint8_t i = 0; i < badge::BTN_COUNT; ++i) {
    if (!buttons.isDown(i)) continue;
    const int n = snprintf(names + used, sizeof(names) - used, "%s%s",
                           used > 0 ? " " : "", badge::buttonName(i));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(names) - used) break;
    used += static_cast<size_t>(n);
  }

  display.linef(kRowButtons, ST77XX_WHITE, "held: %s", used > 0 ? names : "-");
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // Native USB enumerates after the sketch starts, so a short grace period
  // means you actually see the boot banner instead of missing it. This is a
  // bounded wait, not a "wait for a host" spin — the badge must boot fine on
  // AA batteries with no USB attached at all.
  const uint32_t serialDeadline = millis() + 800;
  while (!Serial && millis() < serialDeadline) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=== HTN 2026 badge bring-up ==="));

  display.begin();
  display.header("badge bring-up");

  leds.begin();
  buttons.begin();

  // §10 checklist: WHO_AM_I check before trusting any reading.
  const bool accelOk = accel.begin();
  display.linef(kRowAccelStatus, accelOk ? ST77XX_GREEN : ST77XX_RED,
                "accel: %s", accelOk ? "ok 0x11" : "ABSENT");
  Serial.printf("accel: %s\r\n", accelOk ? "ok" : "absent");

  // Aux1 is a maintained switch and may boot held (§3). Buttons::begin()
  // primed the debouncer from a real read, so surface that initial state here
  // instead of letting it look like a press the user just made.
  if (buttons.isDown(badge::BTN_AUX1)) {
    Serial.println(F("note: Aux1 is held at boot (maintained switch)"));
  }

  display.line(kRowHint, "serial: type help", ST77XX_CYAN);
  renderPressedButtons();

  console.begin(&buttons, &leds, &accel, &display);
}

void loop() {
  const uint32_t now = millis();

  // Serial first: if the rest of the loop ever wedges, the console is how you
  // find out what state it wedged in.
  console.poll();

  if (buttons.poll()) {
    if (buttons.pressedEdges() != 0 || buttons.releasedEdges() != 0) {
      renderPressedButtons();

      for (uint8_t i = 0; i < badge::BTN_COUNT; ++i) {
        if (buttons.wasPressed(i)) {
          Serial.printf("down: %s\r\n", badge::buttonName(i));
          display.linef(kRowLastEdge, ST77XX_YELLOW, "last: %s down",
                        badge::buttonName(i));
        } else if (buttons.wasReleased(i)) {
          Serial.printf("up:   %s\r\n", badge::buttonName(i));
          display.linef(kRowLastEdge, ST77XX_YELLOW, "last: %s up",
                        badge::buttonName(i));
        }
      }
    }
  }

  // Dim, so it cannot contribute to the §6 brown-out. Also doubles as the
  // liveness tell: if the chase freezes, loop() is blocked.
  leds.chase(now);

  if (now - lastAccelMs >= 200) {
    lastAccelMs                 = now;
    const badge::AccelReading r = accel.read();
    if (r.valid) {
      display.linef(kRowAccelValues, ST77XX_WHITE, "x%5d y%5d z%5d", r.x_mg,
                    r.y_mg, r.z_mg);
    }
  }

  if (now - lastClockMs >= 1000) {
    lastClockMs = now;
    display.linef(kRowUptime, ST77XX_WHITE, "up %lus  heap %luk",
                  static_cast<unsigned long>(now / 1000),
                  static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  }
}
