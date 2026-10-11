# badge-accel

Accelerometer-only firmware for the HTN 2026 hacker badge (ESP32-C3-MINI-1-N4).
Reads the SC7A20 at 100 Hz and streams samples as CSV over USB. Nothing else is
touched — no screen, no LEDs, no NFC, no BLE.

Hardware details come from the official HAL guide
(<https://badge.hackthenorth.com/custom-flash>); the local copy with notes is at
`~/.claude/reference/htn-badge-esp32-c3.md`.

> **Status: flashed and verified on hardware 2026-10-11.** `WHO_AM_I` returned
> `0x11`, 970 samples came back over 10 s at 97.8 Hz with zero dropped-sample
> gaps, and gravity measured 1082 mg at rest (see *Calibration* below).

This unit's **Start button and power switch are both broken**, so download mode
cannot be entered by holding Start. Use esptool's USB-Serial-JTAG reset with the
AA battery removed — see `RESTORE.md`.

## Flashing

1. Arduino IDE → Boards Manager → install **esp32** (Espressif).
2. Tools settings:
   - Board: **ESP32C3 Dev Module**
   - **USB CDC On Boot: Enabled** ← without this `Serial` output goes nowhere
   - Upload Speed: 921600
3. Open `badge_accel/badge_accel.ino`, pick the badge's port, upload.
4. If upload fails with *"No serial data received"*: **hold the Start button
   (GPIO9) while plugging in USB-C**, then upload again. Native USB has no
   auto-reset, so this hold is normal. A blank screen in that state is download
   mode, not a brick.

Re-flashing wipes the event firmware. That is a one-way door unless you have a
backup of it.

## Reading the data

```sh
python read_accel.py                  # auto-detect the port, print to stdout
python read_accel.py --out accel.csv  # also append to a file
python read_accel.py --list           # show candidate ports
```

Only one process can own the port — close the Arduino Serial Monitor first.

## Data format

```
n,t_ms,ax_mg,ay_mg,az_mg
0,1432,-12,8,1004
1,1442,-9,11,998
```

- `n` — sample counter, so you can spot dropped samples
- `t_ms` — `millis()` on the badge at read time
- `ax/ay/az` — **milli-g**, signed. At rest one axis reads ≈ ±1000 (gravity).
- Lines starting with `#` are status/errors, not data.

Sample rate is 100 Hz (`CTRL_REG1 = 0x57`), range ±2 g (`CTRL_REG4 = 0x80`).
The sketch polls `STATUS` for the ZYXDA flag and only reads when a new sample is
ready, so it does not alias or duplicate samples.

## Register notes

Each axis arrives as a 12-bit value left-justified in an int16, so the sketch
shifts right by 4 to get counts; at ±2 g one count is 1 mg. I2C calls use
bounded timeouts, so a wedged bus drops samples instead of hanging the loop —
that matters on this board because the NFC chip shares the bus and can drag it
down on low battery.

## Native USB will stall your loop

`Serial.setTxTimeoutMs(0)` is not optional on this board. By default every
`Serial` write blocks waiting for a host to drain the USB-CDC FIFO. With a
laptop reading the port you see 97.8 Hz and everything looks perfect; unplug the
reader and the loop collapses to **0.7 Hz** and the display appears frozen.

Measured on this badge, same test both ways:

| Condition | Rate |
|---|---|
| Host attached, default timeout | 97.8 Hz |
| Detached, default timeout | **0.7 Hz** |
| Detached, `setTxTimeoutMs(0)` | 97.8 Hz |

Any badge firmware that prints over USB and must run untethered needs this,
including anything built from the HAL guide. Verify it with `detach_test.py`,
which times the loop against the badge's own `millis()` across a window with
the port closed. Measuring the rate with a reader attached cannot detect this
failure.

Note that closing the serial port resets the board (DTR/RTS drop on close), so
expect an uptime reset whenever a reader disconnects.

## Display

The screen shows `compiled-15` in large white text, centred, on black. It is
drawn once at boot and never redrawn, so it never competes with the 100 Hz
accelerometer polling in `loop()`.

- Driver: Adafruit ST7789 over SPI2 at 40 MHz (MOSI=10, CLK=1, CS=2, DC=0, RST=4)
- `tft.init(240, 320)` then `setRotation(3)` gives a 320x240 landscape canvas
- `invertDisplay(true)` is required on this panel, per the badge HAL guide
- **`LCD_ROTATION = 3`** on this unit. The guide's `swap_xy(true)` +
  `mirror(true,false)` maps to Adafruit rotation 1, but that renders upside down
  on this badge, so 3 (a 180 degree turn) is the correct value here. If you ever
  see it flipped again, that constant is the only thing to change (0-3).
- Panel confirmed to light and render, so there is no separate backlight pin to
  worry about despite the HAL guide's pin map not listing one.

To change the text, edit the `BANNER` constant. At text size 4 each character is
24 px wide, so roughly 13 characters fit across 320 px before it clips.

## Live chart

The bottom of the screen is a sweep chart of acceleration magnitude:

- Scale 0 to 3000 mg, grey line at 1 g rest, orange line at the 1600 mg bump
  threshold; the trace turns red above it
- Oscilloscope-style: a 4-column gap runs ahead of the cursor, one column drawn
  per sample, so a full sweep takes about 3 seconds
- Drawing costs nothing measurable - the sample rate is 97.8 Hz with the chart
  running, the same as without it

Measured over 25 seconds of real bumps: rest sits at 1082 mg, bumps peak between
1600 and 3063 mg, so the threshold separates them cleanly with no false
positives at rest. Tune `MG_BUMP` if your gesture is softer or harder.

**Known limitation.** Bump reporting is level-triggered with a 250 ms rate
limit, not edge-triggered. Sustained shaking re-fires every 250 ms rather than
reporting one event per bump, so `BUMP` currently means "above threshold now".
Anything that records discrete events (a database write, for instance) needs
edge detection with hysteresis: fire on the upward crossing, re-arm only after
the magnitude falls back under roughly 1250 mg.

## Calibration

At rest the measured magnitude was **1082 mg**, about 8% above the ideal 1000.
That is normal for an uncalibrated consumer part — it folds together zero-g
offset and sensitivity tolerance. The mean resting vector was
`ax=-6, ay=199, az=1063 mg`, i.e. the badge was lying slightly tilted.

If you need absolute accuracy rather than relative motion, correct it: record
the mean of each axis in six orientations (+/-X, +/-Y, +/-Z facing down), then
per axis take offset as the midpoint of the two extremes and scale as
`1000 / ((max - min) / 2)`. For gesture, shake, tap, or orientation work the raw
numbers are fine as they are.
