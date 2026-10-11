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
#include "BumpDetector.h"
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
badge::BumpDetector detector;
badge::Console console;

// Screen rows, named so the layout is editable without counting.
constexpr uint8_t kRowAccelStatus = 0;
constexpr uint8_t kRowAccelValues = 1;
constexpr uint8_t kRowButtons     = 2;
constexpr uint8_t kRowLastEdge    = 3;
constexpr uint8_t kRowUptime      = 4;
constexpr uint8_t kRowKnocks      = 5;
constexpr uint8_t kRowHint        = 6;

uint32_t lastAccelMs = 0;
uint32_t lastClockMs = 0;

// Knock feedback runs on a deadline instead of a delay(): §0 says long compute
// blocks the UI loop, and a blocking flash would also stall the console. While
// a flash is up the LED chase is suspended so the two do not fight.
uint32_t flashUntilMs = 0;
uint32_t knockCount   = 0;

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

  // The knock detector takes over the accelerometer completely: it samples at
  // 400 Hz from its own task, so nothing else may touch the bus afterwards.
  // Everything that wants a reading asks detector.lastReading() instead.
  const bool bumpOk = accelOk && detector.begin(&accel);
  display.linef(kRowKnocks, bumpOk ? ST77XX_GREEN : ST77XX_RED, "knocks: %s",
                bumpOk ? "0" : "detector off");
  Serial.printf("bump detector: %s\r\n", bumpOk ? "running at 400 Hz" : "off");
  if (bumpOk) {
    Serial.println(F("note: run 'bump flip' -- one axis must read ~+/-1000 mg"));
    Serial.println(F("      at rest. ~250 means the HR/full-scale write failed."));
  }

  display.line(kRowHint, "serial: type help", ST77XX_CYAN);
  renderPressedButtons();

  console.begin(&buttons, &leds, &accel, &display, &detector);
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

  // Knock events. The detector already enqueued these from its own task, so
  // this is pure presentation — the feedback latency a user feels is the queue
  // drain, not the detection.
  badge::BumpEvent ev;
  while (detector.poll(&ev)) {
    ++knockCount;
    // One NDJSON object per line, starting at column 0 with '{'. That single
    // character is the whole protocol the host-side bridge filters on; every
    // other line this firmware prints is human-facing.
    Serial.printf("{\"seq\":%lu,\"t_ms\":%lu,\"mg\":%ld,\"axis\":%u,\"flags\":%u}\r\n",
                  static_cast<unsigned long>(ev.seq),
                  static_cast<unsigned long>(ev.t_ms),
                  static_cast<long>(ev.peak_mg), ev.axis, ev.flags);

    display.linef(kRowKnocks, ST77XX_GREEN, "knocks: %lu  %ld mg",
                  static_cast<unsigned long>(knockCount),
                  static_cast<long>(ev.peak_mg));

    // Dim on purpose (§6 brown-out budget), and colour-coded by axis so the
    // demo shows which face was struck.
    const uint8_t r = ev.axis == bump::kAxisX ? 40 : 0;
    const uint8_t g = ev.axis == bump::kAxisY ? 40 : 0;
    const uint8_t b = ev.axis == bump::kAxisZ ? 40 : 0;
    leds.setAll(r, g, b);
    leds.show();
    flashUntilMs = now + 90;
  }

  if (flashUntilMs != 0) {
    if (now >= flashUntilMs) {
      flashUntilMs = 0;
      leds.clear();
      leds.show();
    }
  } else {
    // Dim, so it cannot contribute to the §6 brown-out. Also doubles as the
    // liveness tell: if the chase freezes, loop() is blocked.
    leds.chase(now);
  }

  if (now - lastAccelMs >= 200) {
    lastAccelMs = now;
    // Cached copy from the sampling task. Reading the sensor directly here
    // would race that task on the shared I2C bus.
    const badge::AccelReading r = detector.lastReading();
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
