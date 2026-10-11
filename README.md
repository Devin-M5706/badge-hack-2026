# badge-hack-2026

Firmware starter for the **Hack the North 2026 hacker badge** (ESP32-C3).
Clone it, flash it, and the screen, buttons, LEDs and accelerometer are already
working — so an 8-hour hack starts on your idea instead of on peripheral
bring-up.

Built on PlatformIO + Arduino. Every pin number and register value comes from
the official HAL guide and is cited in the code.

---

## Knock — what got built on it

A badge that tells a deliberate knock from someone else's table slam, decided
on the device in fixed-point integers.

Any accelerometer can tell you something hit it. The useful part is knowing it
was you, and that is the part that is hard. Four tests run in order, cheapest
first, and the third one does the real work.

![Knock detection pipeline](diagrams/knock-detection-pipeline.png)

A knuckle strike has a 0.3 to 2 ms contact time, far above any rate this sensor
reaches, so the detector never samples the impact. It samples the envelope of
momentum transfer. A knock dumps energy into damped tissue or a massive desk
and dies in 10 to 40 ms. A desk is a plate with 20 to 100 Hz bending modes that
rings for 50 to 200 ms while the badge rides along. Both decay, so decay is not
the separator. How many times the signal comes back up is.

**Measured, not asserted:**

| | |
|---|---|
| 2,707 recordings of real human motion, 14.5 hours | **0 false knocks** |
| 1,798 real falls, peaks to 19.8 g | **0 false knocks** |
| Host tests in CI | **39 passing** |
| Detector cost | 6 to 12% of one core at 400 Hz |

With isolation and agitation both disabled, 120 fall recordings armed 460
candidates and the ringdown test rejected all 460, at peaks up to 13 times the
threshold. Amplitude was never what stopped them.

**It also found a contradiction in the official HAL guide.** That document says
to write `CTRL_REG4 = 0x80` and describes the output as 12-bit at 1 mg per
count. Bit 3 is HR, and `0x80` leaves it clear, which is 10-bit mode where the
value moves in steps of 4. Both cannot be true. Confirmed against the Silan
SC7A20 datasheet Rev 1.3, which publishes no sensitivity figure at all for
non-HR mode. The correct value is `0xA8`, and the flip test on real silicon
reads 1064 mg where a failed write would read 250. See
[docs/SC7A20-REGISTERS.md](docs/SC7A20-REGISTERS.md).

Full write-up, including the open-data replay and the research that calibrates
these numbers: **[docs/KNOCK.md](docs/KNOCK.md)**.

The firmware in the demo video is [badge-accel/](badge-accel/), a single
Arduino sketch. `firmware/` is the tested version with the register fix. The
two differ on purpose and [badge-accel/README.md](badge-accel/README.md) says
how.

---

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
  src/main.cpp            bring-up sketch + the knock detector wired in
  lib/ButtonLogic/        pure logic, no Arduino, tested on your laptop
  lib/BumpLogic/          the knock detector, same deal: no Arduino, 19 tests
  lib/Badge/              Buttons, Leds, Display, Accel, Console, BumpDetector
  test/                   host-side tests, run in CI
badge-accel/              the Arduino sketch in the demo video (logo, chart, LEDs)
host/                     USB-serial bridge to a MongoDB Atlas time series
tools/                    dataset replay, labelled capture, model training
diagrams/                 diagram sources, rendered SVG and PNG
docs/HARDWARE.md          pin map, flashing, and the gotchas worth reading first
docs/KNOCK.md             the knock detector write-up
docs/SC7A20-REGISTERS.md  register-by-register truth, labelled by evidence
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
