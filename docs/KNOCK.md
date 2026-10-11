# Knock — turning a conference badge into a physical input device

Built at **c0mpiled 15** (Penn State, 2026-10-11) on a re-flashed **Hack the North
2026 hacker badge** (ESP32-C3 + SC7A20 accelerometer).

You knock the badge. It knows. Every knock becomes a timestamped measurement in a
**MongoDB Atlas time series collection**, and a live page re-renders off a change
stream as you hit it.

The hard part is not sensing an impact. It is sensing *your* impact and nothing else,
on a 30 g board hanging from a lanyard on a walking person, in a room full of people
slamming tables — using a polled accelerometer on a single-core MCU that shares its
I²C bus with an NFC reader that can wedge it.

---

## What's here

```
firmware/badge_bump/     Arduino sketch: the detector, the serial console, LED feedback
bridge/                  Node: USB-serial -> Atlas time series + live SSE page
  init-db.mjs            creates the collections (run once -- see the warning below)
  bridge.mjs             the bridge, the aggregation, the page
  seed.mjs               replays synthetic knocks: the DB demos with no hardware
scripts/backup-firmware.ps1   dump the badge's stock firmware BEFORE overwriting it
docs/                    the full engineering record (see "Where the detail lives")
```

## Run it

```bash
# 0. BACK UP THE BADGE FIRST. Limited quantity, no guaranteed replacement.
powershell -ExecutionPolicy Bypass -File scripts\backup-firmware.ps1

# 1. database
cd bridge && npm install
cp .env.example .env          # fill in MONGODB_URI
npm run init                  # creates bump_events (TIME SERIES) + badges
npm run seed                  # optional: works with no badge attached

# 2. firmware -- Arduino IDE, board "ESP32C3 Dev Module"
#    >>> Tools -> USB CDC On Boot: ENABLED <<<   (or Serial goes to UART0,
#    whose pins go nowhere on this board, and the badge looks dead)
#    Flash Size 4MB, Flash Mode DIO, 80MHz. Upload firmware/badge_bump.

# 3. bridge
npm run bridge                # then open http://localhost:8080
```

---

## How the detection works

**Configuration first, because the badge's own HAL guide is wrong here.** The guide
says to write `CTRL_REG4 = 0x80` and describes the output as "12-bit, 1 mg/count."
Those cannot both be true: bit 3 is **HR**, and `0x80` leaves it clear, which is
10-bit mode — the value then only moves in steps of 4. We write **`0xA8`** (BDU |
±8 g | HR) and **`0x77`** (400 Hz). Verified against the Silan SC7A20 datasheet
Rev 1.3, which publishes no sensitivity figure at all for non-HR mode.

Why ±8 g and not the guide's ±2 g: flat on a table, Z already carries +1 g of
gravity, leaving **1 g of dynamic headroom on the exact axis a knock excites
hardest**. A knuckle knock reads 3–5 g. At ±2 g it rails, and a knock becomes
indistinguishable from a hard table slam.

Why 400 Hz and not the guide's 100 Hz: we are not sampling the impact — a knuckle
strike on FR4 has a 0.3–2 ms contact time, far above any reachable rate. We sample
the **envelope of momentum transfer**, which lasts 10–40 ms in hand and only
**5–15 ms flat on a desk**. At 100 Hz a desk knock is *one sample*, and every
discriminator except raw amplitude becomes impossible.

**Then four tests, in order.** Gravity is removed per-axis with a shift-based EMA
high-pass (τ ≈ 256 ms, deliberately *below* walking cadence so the vetoes can still
see walking), and the decision variable is the L1 norm — no `sqrt` in the hot path.

| Test | What it rejects |
|---|---|
| **Amplitude** ≥ 1500 mg | Walking. Footfall at the chest is 125–300 counts against a 375 threshold — amplitude, not isolation, is what kills gait |
| **Isolation** — leaky quiet credit | Pre-existing motion. Made *leaky* rather than a hard reset, so a blip 150 ms before a real knock costs 20 ms of credit, not the gesture |
| **Ringdown** — ≤10 of 30 samples hot, then 5 consecutive quiet | A neighbour slamming the desk. This is the one that matters: a knock dumps energy into damped tissue and dies in 10–40 ms, while a desk is a low-damping plate that **rings for 50–200 ms**. The separator is not "does it decay" — both do — it is *how many times it comes back up* |
| **Refractory** — 250 ms, plus a separate 100 ms post-*reject* lockout | Double-firing. The reject lockout is the subtle one: without it, a ringdown poking above threshold five times gets five independent chances to pass by luck |

An honest note on what we got wrong first: the initial design leaned on isolation and
decay. Isolation **cannot** kill walking — the quiet gap between footfalls is
350–450 ms, so any window short enough to be usable is satisfied on every stride.
And strict decay rejects real lanyard knocks, because the badge bounces off your
chest 20–60 ms later. Ringdown replaced decay; amplitude took over from isolation.

## Measured results

Run on a lanyard, on AA batteries, with the LEDs active — not held still at a desk on
USB power.

| Test | Bar | Measured |
|---|---|---|
| Deliberate knocks in hand, 30 trials | ≥27/30, zero double-fires | _TODO_ |
| Lanyard, standing, 30 | ≥27/30 | _TODO_ |
| Flat on desk, 20 | ≥17/20 | _TODO_ |
| Lanyard, walking 3 min | ≤1 false positive | _TODO_ |
| On desk while typing, 2 min | 0 false positives | _TODO_ |
| Neighbour slams the desk ×20 | ≤1 false positive | _TODO_ |
| Under LED load, on battery | `hz` ≥ 380, `gap` ≤ 15 ms | _TODO_ |

**These are demo-grade bars, and the gap is worth stating plainly.** ≤1 false
positive per 3 minutes is ~0.33/min. A product bar is ≤1/hour — about 20× better —
and is not reachable in an afternoon with a polled accelerometer and no labelled
ground truth. For scale, the best trained classifier in Wu et al.'s
smartphone-vibration study reaches 88.5% precision at 75% recall (see Related
work); a ≥27/30 detection bar is the same order of magnitude, not an improvement
on it. The firmware reports its own sample rate and worst scheduling gap over
serial (`s`), so starvation is a number you read rather than a thing you suspect.

## Related work — and why the design looks the way it does

Detecting a brief physical impact from a cheap sensor on a moving platform is a
studied problem, mostly under road-surface monitoring. Kim et al.'s review
sorts automated pothole detection into exactly three families — **vision-based,
vibration-based, and 3D-reconstruction-based** — and that taxonomy is the
cleanest way to explain this badge's constraints, because two of the three are
closed to it.

**Vision is the strongest branch and the badge cannot use it.** Ruseruka et al.
train a YOLO detector on built-in vehicle cameras and report 93.0% precision,
91.6% recall, 87.0% F1 and 96.3% mAP, explicitly to avoid the cost of
"specially designed instrumented vehicles." Those are better numbers than
anything here — and they need a camera and a GPU-class inference budget. This
badge has an ESP32-C3, ~400 KB of SRAM, no PSRAM, and no camera at all. 3D
reconstruction is further out of reach for the same reason. **Vibration is the
only family the hardware admits**, which is why the accelerometer is the sensor
and not a fallback.

**And the vibration branch is where the honest performance numbers live.** Wu
et al. instrument smartphones' built-in vibration sensors, extract time- and
frequency-domain features, and compare machine-learning classifiers; their best
model, a Random Forest, reaches **88.5% precision and 75% recall**. That is a
trained model, on labelled data, with frequency-domain features, on a problem
closely analogous to this one — and it is the number that calibrates this
project's claims. A fixed-threshold detector tuned by one person in an
afternoon, reporting *better* than a peer-reviewed Random Forest on the same
class of problem, should be read as a red flag rather than a result. The
acceptance bars in this README were chosen to sit in that register.

The same literature also motivates the specific signal choices. Wu et al. find
time- *and* frequency-domain features both carry information, which is the
practical argument for not sampling at the badge HAL guide's 100 Hz: a
50 Hz Nyquist limit discards the band where an impact's structure lives, and on
a desk the whole envelope is 5–15 ms wide. Hence 400 Hz, the part's maximum.

> Kim, Y.-M.; Kim, Y.-G.; Son, S.-Y.; Lim, S.; Choi, B.-Y.; Choi, D.-H.
> "Review of Recent Automated Pothole-Detection Methods." *Applied Sciences*
> 2022, 12(11), 5320. <https://doi.org/10.3390/app12115320> (open access)
>
> Wu, C.; Wang, Z.; Hu, S.; Lépine, J.; Na, X.; Ainalis, D.
> "An Automated Machine-Learning Approach for Road Pothole Detection Using
> Smartphone Sensor Data." *Sensors* 2020, 20(19), 5564.
> <https://doi.org/10.3390/s20195564> (open access)
>
> Ruseruka, C.; Mwakalonge, J.; Comert, G.; Siuhi, S.; Ngeni, F. C.; Anderson, Q.
> "Augmenting roadway safety with machine learning and deep learning: Pothole
> detection and dimension estimation using in-vehicle technologies."
> *Machine Learning with Applications* 2024, 16, 100547.
> <https://doi.org/10.1016/j.mlwa.2024.100547> (open access)

**One thing this work does not claim.** None of the above validates a
threshold-and-debounce detector as *better* than a learned one. The published
vibration results use trained classifiers, and with labelled data this project
probably should too. Thresholds were chosen here because there is no labelled
dataset of knocks on this badge and no time to build one — a constraint, stated
as a constraint.

## Why MongoDB is load-bearing here

Not "we stored events in it." Three specific things:

1. **Time series collection.** Knocks are measurements with a timestamp and a
   low-cardinality source — the shape time series collections exist for. Automatic
   bucketing, a 24-hour TTL, and an index on `{meta.badgeId, ts}`.
2. **The aggregation makes silence visible.** `$densify` invents the 5-second buckets
   where nothing happened, `$fill` carries zero into them, and `$setWindowFields`
   computes a rolling 60-second knock rate. A plain `find()` can show you knocks; it
   cannot show you the *absence* of knocks, and for a rhythm the gaps are the signal.
3. **A change stream drives the live page.** Which forced a real schema decision:
   time series collections support neither change streams nor triggers nor `$search`,
   so the measurements live in `bump_events` and a small regular `badges` collection
   carries the rollups the stream watches.

## Known limitations

- **Sitting down is the hardest false positive** — still, then descend, then impact.
  It passes both isolation and decay, and we do not fully solve it.
- **No hardware tap engine.** The SC7A20 advertises single/double-click detection,
  but its datasheet is truncated exactly where that register block would be defined
  (`0x38`–`0x3D` are named in the address map and never described), and no public
  driver touches it. We use the datasheet-proven data path and detect in software.
- **The accelerometer is polled**, because the badge routes neither INT1 nor INT2 to
  a GPIO. Costs 6–12% of one core at 400 Hz.
- **Double-tap is deliberately absent.** Detecting a second tap needs the refractory
  below ~150 ms, which reopens double-firing on *single* taps, and every single tap
  would have to wait out a ~300 ms window — latency from ~65 ms to ~360 ms for every
  user, always. The event carries `t_us` and `seq`, so laptop-side code can group
  taps later with zero firmware change.
- The badge is on USB serial, not Wi-Fi. Deliberate: venue Wi-Fi is WPA2-Enterprise
  or a captive portal, and Atlas App Services (the Data API, HTTPS endpoints, Device
  Sync) reached end of life on 2025-09-30, so there is no endpoint for a
  microcontroller to POST to even in principle.

## Where the detail lives

| Doc | What's in it |
|---|---|
| `docs/BRIEF.md` | The build plan, the schedule, and the arguments against it |
| `docs/bump-detector-design.md` | Full detector derivation, parameter table, 45-minute tuning loop |
| `docs/sc7a20-registers.md` | Register-by-register truth, labelled by evidence strength |
| `docs/mongodb-track-plan.md` | Schema, pipelines, free-tier limits, deprecation research |
| `docs/badge-hardware-spec.md` | The badge's own HAL spec (pins, flashing, peripherals) |
| `docs/idf-windows-runbook.md` | ESP-IDF path, if you ever want it instead of Arduino |
| `docs/datasheets/` | SC7A20 Rev 1.3, LIS3DH, ST AN3308 |

## Serial console

`<key>\r` reads, `<key><number>\r` writes. Units are milliseconds; internals are loop
iterations. Thresholds persist to NVS (`S`), which matters because every reflash needs
a manual download-mode hold and a brownout would otherwise revert everything.

```
t<counts> threshold    q<counts> quiet gate    n<ms> quiet credit   r<ms> refractory
d<n> decay shift       w<ms> ringdown window   h<n> max hot         p<n> blip penalty
a<0|1> agitation veto  s stats   l live telem  c ring CSV   f flip test
z zero+reseed          S save    L load        D defaults   F toggle 8g/2g   ? help
```

`f` is the one to run first: at ±8 g each axis must read **≈ ±250 counts** at rest.
If it reads ≈1000, the HR/full-scale write did not take.
