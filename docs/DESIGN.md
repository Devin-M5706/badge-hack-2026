# Design decisions

Why this repo is shaped the way it is. Written at setup time, before the hack,
so the reasoning survives contact with hour 6.

## Goal

An 8-hour hardware hack on the HTN 2026 badge. The repo exists to make hour 0
productive: clone, flash, and the screen, buttons, LEDs and accelerometer
already work. Everything else is in service of committing cleanly while moving
fast.

## Decision: Arduino framework, not ESP-IDF

The HAL guide calls ESP-IDF "the path of least resistance" (§1.6). We went
Arduino anyway. The reasoning:

The guide's recommendation is about **where the values were verified**, not
about framework capability. Everything it gives you is register- and pin-level:
the HC165 latch-and-shift sequence, `CTRL_REG1=0x57`, the `WHO_AM_I=0x11`
check, the auto-increment bit, GRB ordering, the ST7789 invert/swap/mirror
trio. All of that expresses directly in Arduino `Wire`/`SPI`/`digitalWrite`.
Nothing in §2–§7 needs ESP-IDF.

Against that, Arduino buys iteration speed that matters on an 8-hour clock:
`Adafruit_ST7789` and `Adafruit_NeoPixel` give working screen and LEDs in a few
lines, where the same bring-up in ESP-IDF is `esp_lcd` panel handles, DMA stripe
configuration and an `led_strip` RMT driver.

PlatformIO rather than the Arduino IDE sketchbook, because `platformio.ini`
pins the board, framework **and library versions** in one committed file. With
several people on a team, floating library versions are how you lose an hour at
3am to an API that changed underneath you.

### What this costs, specifically

Three places where Arduino is worse here, all known going in:

1. **BLE is effectively unavailable.** §8 says stock NimBLE buffer tuning OOMs
   this board's heap, and the fix is compile-time sdkconfig trimming that the
   Arduino core does not expose. This is the one real one-way door, which is
   why `platformio.ini` carries a commented `[env:badge_idf]` stanza. **If BLE
   enters scope, switch frameworks rather than fighting it.**
2. **§4's "do not allocate full-frame DMA buffers" becomes a trap** rather than
   a default. Arduino GFX canvas objects silently allocate 150 KB. Mitigated by
   `Display` drawing directly and clearing single rows, and stated as a rule in
   `CLAUDE.md` and `docs/HARDWARE.md`.
3. **Flash layout is not automatic.** Stock Arduino partition schemes do not
   match §0's documented app partition. Mitigated by a committed
   `partitions.csv`.

The escape hatch is cheap because of how the code is split: `badge_pins.h` and
`lib/ButtonLogic` are framework-independent and carry over unchanged. Only
`lib/Badge` would be rewritten.

## Decision: split logic from hardware

`lib/ButtonLogic` has no Arduino includes and is tested on the host.
`lib/Badge` touches pins and is not tested.

This exists because of the two failures the guide explicitly warns about, both
of which are silent on hardware — the board looks alive and the buttons simply
do the wrong thing:

- **Shift order.** §3 specifies `A, B, Home, Down, Left, Right, Up, Aux1`, A
  first, active-low. Transcribe it backwards, or forget the inversion, and
  every button works while being wrong. Note `Down` precedes `Up`; the
  intuitive ordering inverts the d-pad.
- **Aux1 boots held.** §3 notes Aux1 is a maintained side switch, not
  momentary. Without handling, the first poll reports a press the user never
  made, and anything keyed to that edge starts in the wrong state. Hence
  `Debouncer::prime()`, which adopts the boot state without emitting edges.

Both are now proven by host tests instead of discovered on a bench.

## Decision: promote the serial console to first-class

§9 mentions almost in passing that you should "keep at least a button-inject +
print path over USB-Serial-JTAG". It is the highest-leverage line in the
document. Being able to type `btn up` and `state` from a laptop means app logic
gets debugged without physically holding a 2 mm button, and `state` exposes the
pre-inversion raw shift byte, which separates a wiring fault from a decode bug
in one look.

So it is `lib/Badge/Console`, not a scratch `Serial.print` in `main.cpp`.

## Decision: commit hygiene with zero new dependencies

Conventional Commits enforced by a POSIX shell `commit-msg` hook, not husky and
commitlint. A firmware repo should not need a JavaScript toolchain to validate a
string, and anything requiring `npm install` before you can commit gets
bypassed under time pressure.

`pre-commit` additionally blocks build artifacts and files over 2 MiB, because
a committed binary blob is painful to remove from history later.

CI compiles the firmware and runs the host tests on every push. In a timed hack
the worst possible discovery is that `main` stopped building three commits ago.
Formatting is reported but never blocks a merge — a red X over brace placement
at hour 6 only teaches people to ignore CI.

## Scope: deliberately not built

No host-side web app, dashboard, release automation, or deploy pipeline. The
project idea is not fixed yet, so anything above the firmware layer would be
guesswork that gets deleted. If a companion app turns out to be needed, it gets
added as `host/` on the day.

## Testing philosophy

Tests cover logic that is easy to get silently wrong and expensive to debug on
hardware: bit order, sign handling, debounce state, boot-time edge cases.

Tests do not attempt to cover what only hardware can answer. Nobody can
unit-test whether the screen is upright. That is what the bring-up sketch, the
LED chase as a liveness tell, and the serial console are for.
