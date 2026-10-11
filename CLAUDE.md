# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

Firmware for the Hack the North 2026 hacker badge (ESP32-C3-MINI-1-N4).
PlatformIO + Arduino framework. Built for an 8-hour hackathon, so bias toward
working code over architecture.

## Commands

```bash
cd firmware
pio run -e badge        # compile
pio test -e native      # logic tests, no hardware needed
pio run -e badge -t size

scripts/flash.ps1       # flash (Windows); scripts/flash.sh elsewhere
```

If `pio` is not on PATH, use `python -m platformio` instead.

## Hard rules

**Read `docs/HARDWARE.md` before writing code for any peripheral.** Pin numbers
and register values there and in `firmware/include/badge_pins.h` come from the
official HAL guide and are the source of truth. Do not substitute values from
generic ESP32 tutorials — this board uses none of the default SPI/I2C pins.

**Never add a GPIO number outside `firmware/include/badge_pins.h`.** One pin,
one definition.

**Never allocate a full-frame framebuffer.** 320×240×2 = 150 KB against ~400 KB
SRAM with no PSRAM. `Display` draws directly and clears one row at a time. Do
not introduce a screen-sized `GFXcanvas16`.

**No `#include <Arduino.h>` in `lib/ButtonLogic`.** That library is pure logic
so it can be tested on the host. Hardware access belongs in `lib/Badge`.

**Cite the HAL guide section when transcribing a value from it**, e.g.
`// §5: counts = raw >> 4, 1 count = 1 mg at +/-2 g`.

**Do not block in `loop()`.** The ESP32-C3 is single-core; long compute freezes
the UI and the serial console.

**Keep LED brightness capped.** Six LEDs at full white can brown out the board
on AA power. See `LED_MAX_BRIGHTNESS`.

## Architecture

- `firmware/include/badge_pins.h` — every GPIO and bus constant
- `firmware/lib/ButtonLogic/` — decode + debounce, no Arduino deps, tested
- `firmware/lib/Badge/` — `Buttons` (74HC165), `Leds` (WS2812), `Display`
  (ST7789), `Accel` (SC7A20), `Console` (USB serial debug)
- `firmware/src/main.cpp` — bring-up sketch; project code replaces its `loop()`

The `ButtonLogic` / `Badge` split exists so bit order and debounce edge cases
are proven by host tests rather than debugged on hardware.

## Commits

Conventional Commits, enforced by `.githooks/commit-msg`. Scopes in use:
`buttons leds display accel console ci docs hal`.

## If BLE is needed

Switch to ESP-IDF. Stock NimBLE buffers OOM this board's heap and the fix is
compile-time sdkconfig trimming that the Arduino core does not expose. A
commented `[env:badge_idf]` stanza is in `platformio.ini`; `badge_pins.h` and
`ButtonLogic` carry over, `lib/Badge` gets rewritten.
