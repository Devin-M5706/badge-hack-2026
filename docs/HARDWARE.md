# Hardware reference — HTN 2026 hacker badge

Bench quick-reference. The authoritative document is the official
**"Custom firmware + HAL guide (2026 Hacker Badge)"** from
<https://badge.hackthenorth.com/custom-flash>, and in code the authoritative
copy of every pin number is [`firmware/include/badge_pins.h`](../firmware/include/badge_pins.h).

If a number here ever disagrees with `badge_pins.h`, the header wins and this
file is stale — fix it.

> ⚠️ Re-flashing replaces the event firmware. Badges are limited quantity and a
> bricked one may not be replaceable. If the badge gets hot, smells odd, smokes,
> or acts strange, power it off.

## What you are working with

| Part | Detail |
|---|---|
| MCU | ESP32-C3-MINI-1-N4 — single-core RISC-V, 4 MB flash, native USB-Serial-JTAG |
| RAM | ~400 KB SRAM, **no PSRAM** |
| Display | ST7789, 320×240, RGB565, SPI2 @ 40 MHz |
| Buttons | 8 on a 74HC165 shift register + 1 dedicated Start |
| Accelerometer | SC7A20HTR, I2C `0x19` |
| NFC | MFRC522, I2C `0x26` (same bus) |
| LEDs | 6× WS2812B-2020 |
| Power | AA → MT3608 boost → XC6220 LDO; USB-C for flashing |

## Pin map

| Function | GPIO | Notes |
|---|---|---|
| LCD MOSI | 10 | SPI2, 40 MHz, mode 0 |
| LCD CLK | 1 | |
| LCD CS | 2 | |
| LCD DC | 0 | |
| LCD RST | 4 | |
| I2C SDA | 5 | shared: accel `0x19` + NFC `0x26`, 400 kHz |
| I2C SCL | 6 | |
| HC165 DATA | 7 | shift register serial out |
| HC165 LOAD | 20 | pulse low to latch |
| HC165 CLK | 21 | |
| START | 9 | active-low; **strapping pin** → download mode |
| LED DIN | 3 | WS2812, GRB order |

## Flashing

```bash
scripts/flash.ps1      # Windows
scripts/flash.sh       # macOS / Linux / git bash
```

**If upload fails with "No serial data received":** unplug USB-C, hold **Start**
down, plug USB-C back in while still holding, release, retry.

A **blank screen in this state is expected** — that is download mode, not a
brick. Start is GPIO9, a strapping pin, and this board's native USB has no
auto-reset, so the manual hold is the documented procedure.

Only one process can own the serial port. A serial monitor left running is the
most common cause of a flash that mysteriously stops working.

## Gotchas that cost real time

These are the ones worth reading before you debug, not after.

**Aux1 is a maintained switch, not momentary.** It can already be held at boot.
`Buttons::begin()` primes the debouncer from a real read so this does not look
like a fresh press; `Debouncer::prime()` is what makes that work, and there are
tests for it.

**The console is USB-Serial-JTAG, not UART0.** Under Arduino this needs
`-D ARDUINO_USB_MODE=1` and `-D ARDUINO_USB_CDC_ON_BOOT=1`, both already in
`platformio.ini`. Without them `Serial.print` goes nowhere and the board looks
dead while running perfectly.

**Never allocate a full-frame buffer.** 320×240×2 = 150 KB against ~400 KB of
SRAM and no PSRAM. `Display` draws directly and clears single rows. A
`GFXcanvas16` the size of the screen will not survive.

**Six LEDs at full white can brown out the board on AA power.** Brightness is
capped at `LED_MAX_BRIGHTNESS` (40/255). A brown-out mid-demo looks exactly
like a crash, and you will go hunting for a bug that is a power budget.

**Multi-byte I2C reads need the auto-increment bit.** Set bit 7 of the register
address, or you read the same register six times and the sensor appears broken
while answering correctly every time.

**The NFC chip can wedge the shared I2C bus on droopy battery power.** Use
bounded timeouts, never block forever. `Accel` counts failures — a steadily
climbing `errs` in `state` points at power or bus contention, not your code.

**The ESP32-C3 is single-core.** Long compute blocks the UI loop. If the LED
chase stutters or the console stops answering, something in `loop()` is taking
too long.

**NFC is power-hungry and has no low-power card detect.** If you do not need it,
do not initialise it. If you do, duty-cycle the field (~750 ms on/off) and
suspend accel polling while scanning.

**BLE needs ESP-IDF, not Arduino.** Stock NimBLE buffer sizes OOM this board's
heap, and the fix is compile-time sdkconfig trimming that the Arduino core does
not expose. See the commented `[env:badge_idf]` block in `platformio.ini`.

## Serial console

After flashing, open the monitor and type `help`:

```
cd firmware && pio device monitor -b 115200
```

| Command | Does |
|---|---|
| `state` | buttons held, raw shift byte, accel mg, uptime, free heap |
| `btn <name> [ms]` | inject a press — drive app logic without touching the board |
| `led <i> <r> <g> <b>` | set one LED |
| `leds off` | all off |
| `rot <0..3>` | change display rotation if the image is mirrored |

The inject path is the single highest-leverage debug tool here: you can exercise
menu navigation and game logic from your keyboard instead of holding a 2 mm
button with a fingernail.
