# badge-hack-2026

Firmware starter for the **Hack the North 2026 hacker badge** (ESP32-C3).
Clone it, flash it, and the screen, buttons, LEDs and accelerometer are already
working — so an 8-hour hack starts on your idea instead of on peripheral
bring-up.

Built on PlatformIO + Arduino. Every pin number and register value comes from
the official HAL guide and is cited in the code.

## Quickstart

```bash
git clone https://github.com/Devin-M5706/badge-hack-2026.git
cd badge-hack-2026
scripts/setup.ps1              # Windows   (or: scripts/setup.sh)

cd firmware
pio run -e badge               # compile
pio test -e native             # run logic tests, no board needed
cd ..
scripts/flash.ps1              # flash a connected badge
```

No PlatformIO yet? `python -m pip install --user platformio`

**If the upload fails:** unplug USB-C, hold **Start**, plug back in while still
holding, release, retry. A blank screen then is download mode, not a brick —
the flash scripts print this when they fail, so you do not have to remember it.

## What you get on first flash

The bring-up sketch walks the HAL guide's minimal checklist, so one flash tells
you whether the hardware is sound:

- Panel renders upright, with live text rows
- Every button press and release named on screen and over serial
- Accelerometer reporting in mg, or saying plainly that it is absent
- An LED chase that doubles as a liveness indicator — if it stops, `loop()` is blocked
- A serial console: type `help`

## Layout

```
firmware/
  include/badge_pins.h    every GPIO, in one place
  src/main.cpp            bring-up sketch — replace this with your project
  lib/ButtonLogic/        pure logic, no Arduino, tested on your laptop
  lib/Badge/              Buttons, Leds, Display, Accel, Console
  test/                   host-side tests
docs/HARDWARE.md          pin map, flashing, and the gotchas worth reading first
docs/DEMO.md              demo script — fill this in early
scripts/                  setup and flash wrappers
```

The split between `ButtonLogic` and `Badge` is the one structural idea worth
knowing: anything that can be reasoned about without hardware lives in
`ButtonLogic` and has tests, and anything that touches a pin lives in `Badge`.
That is why shift order and debounce are proven on a laptop instead of being
debugged with a fingernail at 3am.

## The serial console

The most useful thing in here. Flash, open a monitor, type `help`:

| Command | Does |
|---|---|
| `state` | buttons held, raw shift byte, accel mg, uptime, free heap |
| `btn <name> [ms]` | inject a press — drive app logic without touching the board |
| `led <i> <r> <g> <b>` | set one LED |
| `rot <0..3>` | fix a mirrored display |

## Working in here

Commits follow [Conventional Commits](https://www.conventionalcommits.org/),
enforced by a shell hook that `scripts/setup.*` installs — no Node required:

```
feat(buttons): add HC165 bit-bang read
fix(accel): set auto-increment bit on multi-byte reads
```

CI compiles the firmware and runs the logic tests on every push. In a timed
hack the worst discovery is that `main` stopped building three commits ago.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the few rules that matter, and
[docs/HARDWARE.md](docs/HARDWARE.md) before touching a peripheral.

## Changing the hardware target

`platformio.ini` pins the board, framework and library versions on purpose.

**If BLE becomes part of the project, switch to ESP-IDF.** Stock NimBLE buffers
OOM this board's heap, and the fix is compile-time sdkconfig trimming the
Arduino core does not expose. There is a commented `[env:badge_idf]` stanza
ready for it; `badge_pins.h` and `ButtonLogic` carry over unchanged, and only
`lib/Badge` gets rewritten.

## Credits

Pin map and register values from the official
[Custom Flash / HAL guide](https://badge.hackthenorth.com/custom-flash).
That document is the source of truth — this repo is an implementation of it.

MIT licensed.
