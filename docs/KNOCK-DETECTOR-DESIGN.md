# Bump / knock detector — HTN 2026 badge (ESP32-C3 + SC7A20)

**Target:** a deliberate knock on the badge fires **exactly one** event, on a
single-core RISC-V MCU, on an I2C bus shared with an NFC reader that can wedge
it, tuned by one person in ~3 hours on a bench in a loud room.

**Design stance:** the badge guide's accel bring-up (`CTRL_REG1=0x57`,
`CTRL_REG4=0x80` → 100 Hz, ±2 g) is a **tilt/orientation** configuration. It is
the wrong configuration for tap detection and this design changes both numbers.
That is the highest-impact decision in this document, so §1 justifies it first.

**Notation for every number below:**

- **[P]** = physically grounded (derived from mechanics, human factors, or the
  datasheet; change it only if the bench contradicts the physics).
- **[S]** = starting point (a defensible first guess that you are *expected* to
  move during the tuning loop in §6).
- **[U]** = unverified against this specific part; a bench check is given.

---

## 0. Executive summary

| Decision | Value | Why |
|---|---|---|
| ODR | **400 Hz** (`CTRL_REG1 = 0x77`) | Need 4+ samples on the knock *envelope* for a decay test. 200 Hz gives 1–3 on a desk knock. **[P]** |
| Full scale | **±8 g** (`CTRL_REG4 = 0xA0`), 4 mg/count | ±2 g **clips on every knock**, destroying the amplitude discriminator. **[P]** |
| Poll loop | **2 ms task** (500 Hz), no `STATUS` read, 6-byte burst | ~20% duplicate samples, harmless; halves I2C traffic. **[P]** |
| Gravity removal | EMA high-pass, shift 7, Q8 fraction | tau = 256 ms, f_c = 0.62 Hz — kills DC/tilt, **leaves walking visible** for the veto. **[P]** |
| Decision variable | **L1 norm** of the AC part: abs(ax)+abs(ay)+abs(az) | 3 abs + 2 adds, branch-free, linear units for console tuning. |
| Ring buffer | 128 x 8 B = **1 KB**, debug/CSV only | The detector itself needs **zero** buffer — all state is 6 scalars. |
| Primary discriminator | **Amplitude with headroom** | The one the other three depend on. Only works because of ±8 g. |
| Discriminator set | isolation (leaky) + **ringdown** + agitation + refractory | §2. The proposed "decay" test is replaced by a stronger ringdown form. |
| Refractory | **250 ms** | Swallows a human double-knock (~150 ms floor) into one event. **[P]** |
| Queue | depth **4**, drop-newest, counted | §4. |
| Must-not-cut | refractory, NVS config save, health counter, bounded I2C timeout | §7. |

**Two findings that will bite the builder if skipped:**

1. **`CONFIG_FREERTOS_HZ` must be 1000.** At the ESP-IDF default of 100,
   `pdMS_TO_TICKS(2)` evaluates to **0 ticks** and `xTaskDelayUntil` with a zero
   period either asserts or spins the task at 100% CPU, starving the display and
   the LEDs. This is a show-stopper, not a nicety. Side effects are benign and
   mostly good: ~1–2% extra tick overhead, but the 10 ms button poll and display
   timing both get finer granularity.
2. **The tuned config must persist to NVS.** Every reflash costs a manual
   download-mode hold (hold Start while plugging USB). If the threshold is a
   compile-time constant you will reflash 15 times during tuning and then lose
   the values on the next power cycle. The `S`/`L` console commands are **not
   optional polish** — they are what makes the 45-minute tuning loop possible.

---

## 1. The signal chain

### 1.1 ODR: why 400 Hz, and the honest Nyquist answer

**You cannot sample a knock. Accept that first, then design around it.**

A knuckle strike on FR4 has a contact duration of roughly **0.3–2 ms**, so the
forcing spectrum extends to ~500 Hz–3 kHz. The badge's own plate bending modes
for a ~55 x 85 mm populated PCB sit around **300 Hz–2 kHz**. Virtually all of
the true mechanical transient is above any ODR reachable over a 400 kHz I2C bus
shared with an NFC reader. **[P]**

What rescues the design is that the SC7A20, like the LIS2DH/LIS3DH family whose
register map it clones (confirmed: the guide's `CTRL_REG1=0x57` → 100 Hz matches
the LIS3DH ODR table exactly, and `counts = raw>>4` with 1 mg/count at ±2 g
matches 12-bit left-justified HR output), band-limits its output path **before**
sampling, roughly BW ~ ODR/2. The kHz content is therefore **attenuated, not
folded down as alias**. **[U — bench check in §1.7.]**

So what arrives at the register is not the impact; it is the **envelope of the
momentum transfer** plus the ringdown of whatever the badge is coupled to — the
hand, the lanyard-and-chest, or the desk. Those are heavily damped mass-spring
systems in the 20–200 Hz range. Envelope widths: **[P]**

| Coupling | Envelope width | Why |
|---|---|---|
| In hand | 10–40 ms | Flesh damping, zeta > 0.2, dies in 2–3 cycles |
| On lanyard | 15–50 ms | Plus a secondary chest bounce |
| Flat on desk | 5–15 ms | Desk mass absorbs; short and sharp |

The ODR question is therefore **"how many samples land on the envelope"**, not
"can I resolve the transient":

- **100 Hz** (the guide's value, 10 ms/sample): a desk knock is **1 sample**.
  You get amplitude and nothing else. No decay test, no ringdown test, no rise
  measurement. Every discriminator in §2 except raw amplitude becomes
  impossible. **This is why the guide's value must change.**
- **200 Hz** (5 ms/sample): desk knock = 1–3 samples; a 60 ms window holds 12
  iterations. Borderline — the ringdown test loses most of its power exactly in
  the scenario (badge on a desk) where it has to beat a neighbour's table slam.
- **400 Hz** (2.5 ms/sample): desk knock = 2–6, in-hand = 4–16, decision window
  = 30 iterations. The ringdown test becomes meaningful. **Chosen.**
- **1.344 kHz** (normal-mode max): better still, but ~200 µs of blocking I2C per
  sample means ~27% of the bus and hard contention with NFC. Not worth it on a
  single core with a shared bus.

**Verdict: ODR = 400 Hz, `CTRL_REG1 = 0x77`** (ODR bits `0111`, LPen=0, XYZ on).
**[P]**

**Coupled decision, stated plainly:** if the bench shows the sample rate sagging
under display load (§5 health counter), fall back to **200 Hz (`0x67`), poll
4 ms, EMA shift 6** — and accept that the ringdown test is weakened, so
desk-mode slam rejection degrades. The ODR choice and the discriminator choice
are not independent.

### 1.2 Full scale: ±2 g is the wrong answer, and clipping is why

A deliberate knuckle knock produces, **at the accelerometer, away from the
impact site**, roughly **1.5–6 g**. **[P]** At ±2 g every axis rails at 2047
counts, so:

- Every knock reads 2047.
- A hard table slam also reads 2047.
- **The amplitude discriminator — the one the other three depend on — is
  destroyed by saturation.**

The guide picked ±2 g because it is correct for tilt. For impulse detection you
need headroom to *measure* that a knock and a slam differ, not to clip both.

LIS3DH-compatible FS bits are `CTRL_REG4[5:4]`; 12-bit sensitivity runs
1 / 2 / 4 / 12 mg per count for ±2 / ±4 / ±8 / ±16 g.

**Verdict: ±8 g, `CTRL_REG4 = 0xA0`** (BDU=1, BLE=0, FS=`10`) → **4 mg/count,
range 8000 mg.** **[P]**

| Signal | Expected peak | Counts at 4 mg |
|---|---|---|
| Resting in hand / standing still | 20–50 mg | 5–13 |
| Lanyard swing | 200–500 mg | 50–125 |
| Brisk walking, at the chest | 500–1200 mg | 125–300 |
| Deliberate knock | 1500–6000 mg | 375–1500 |
| Hard table slam → badge on desk | 400–3000 mg | 100–750 |

±16 g would survive any slam without clipping, but 12 mg/count leaves the quiet
gate only 2–3 counts wide, which is inside the noise floor. ±8 g is the
Goldilocks point: ~1–2 counts of noise, 375+ counts of knock.

### 1.3 Bring-up gate — the 1 g flip test (30 seconds, do not skip)

SC7A20 is a Silan part, not an ST part. The FS bits *should* behave like the
LIS3DH, but that is **[U]**. There is a trivial empirical check:

> Lay the badge flat, at rest. Read Z. At ±8 g / 4 mg it must read
> **~ +250 counts** (1000 mg / 4). If it reads **~ +1000**, the FS write did not
> take and you are still at ±2 g / 1 mg.

Flip through all six orientations; each axis must hit ~ ±250 at rest. **This
gate comes before any tuning.** If it fails, see §1.8.

### 1.4 Gravity removal — EMA high-pass, shift-based

Per axis, in Q8 fixed point:

```c
int32_t xq = (int32_t)raw_counts << 8;        /* Q8 */
ema += (xq - ema) >> K;                       /* gravity tracker */
int32_t ac = raw_counts - (ema >> 8);         /* high-passed sample */
```

**The classic bug this avoids:** keep the EMA at Q0 with `K=7` and a 100-count
gravity offset produces `100 >> 7 == 0` — **the EMA never moves**. The tracker
stalls permanently and the AC signal is garbage. The rule is **fractional bits
>= K**. With Q8 and K=7 a 1-count error moves the EMA by `256 >> 7 = 2`, so it
always converges.

Time constant for `y += (x-y)>>K` is **tau ~ 2^K / f_loop** (pole at 1 - 2^-K).
It is the **loop** rate, not the ODR — duplicate samples still get fed in, so
use 500 Hz at the 2 ms poll.

| Loop rate | K=5 | K=6 | **K=7** | K=8 |
|---|---|---|---|---|
| **500 Hz** (400 Hz ODR, 2 ms poll) | 64 ms | 128 ms | **256 ms** | 512 ms |
| **250 Hz** (200 Hz ODR, 4 ms poll) | 128 ms | **256 ms** | 512 ms | 1.0 s |

**Verdict: K=7 at 400 Hz, K=6 at 200 Hz — both give tau = 256 ms,
f_c = 1/(2*pi*tau) = 0.62 Hz.** **[P]** (The requested figures stated directly:
at 400 Hz ODR / 500 Hz loop, shift 7 = 256 ms; at 200 Hz ODR / 250 Hz loop,
shift 6 = 256 ms. If you poll exactly at the ODR instead of oversampling,
shift 7 at 400 Hz = 320 ms and shift 6 at 200 Hz = 320 ms.)

Why that corner and not lower or higher:

- **Lower corner (tau >= 1 s)** — gravity stops tracking reorientation. Turn the
  badge over and you get a 2000 mg step that looks exactly like a knock for a
  full second.
- **Higher corner (tau <= 80 ms)** — the EMA starts **chasing the knock itself**.
  With a 10–40 ms envelope and tau = 80 ms the separation is only 2–4x; the
  tracker eats the peak and then manufactures an undershoot that corrupts the
  decay test. At tau = 256 ms separation is 6–25x, so the EMA barely moves
  during an event.
- **Deliberately high enough to still see walking.** f_c = 0.62 Hz is *below*
  walking cadence (1.8–2.2 Hz) and lanyard swing (1–2 Hz). This is intentional:
  the high-pass must **not** remove the motion the vetoes in §2 need to see. A
  5 Hz corner would make walking invisible and the isolation test useless.

**Warm-up:** seed `ema[i]` with the first sample, then blank detection for
**2*tau = 512 ms** (320 iterations at 2 ms). **[P]** Re-seed and re-blank on
**every NFC resume** — see §2.6; this is a real false-positive source.

### 1.5 Decision variable — L1 norm, and why no sqrt

`mag = abs(ac_x) + abs(ac_y) + abs(ac_z)` — 3 abs, 2 adds, branch-free, no
multiply, no sqrt.

L1 overestimates true magnitude by up to sqrt(3) ~ 1.73x for a perfect diagonal
and 1.0x for a single-axis event. Three reasons that is acceptable, even helpful:

1. **Monotonic in the thing we care about.** Thresholding needs ordering only.
2. **The anisotropy is tuned out, not modelled out.** You set the threshold with
   real knocks, so the 1.0–1.73x spread is absorbed into the tuned value. In
   practice knocks hit 2–3 axes, so the real spread is ~1.0–1.4x.
3. **Linear units make the console usable.** Threshold, quiet gate and decay
   gate are all in the same counts, ratios are linear, and printed stats compare
   directly against printed thresholds. With squared magnitude the decay *ratio*
   becomes a ratio of squares and every number a human types needs mental
   arithmetic. In a 3-hour budget that beats 0.3 dB of isotropy.

**The anisotropy becomes a measured claim, not a hope:** §6 test A knocks the
face, both edges and the back, and each must pass independently. If edge knocks
fail while face knocks pass, that *is* the anisotropy showing up, and the fix is
a lower threshold, not a different norm.

**If you prefer squared magnitude** (3 MULs + 2 adds; the RV32IMC hardware
multiplier makes this ~10 cycles, genuinely free at 400 Hz): precompute
`thr2 = thr*thr` on every console change, compare `mag2 >= thr2`, and square the
decay ratio too (`dgate2 = thr2 >> (2*dshift)`). Everything else in §2 is
unchanged. Convert a tuned L1 threshold with `thr_L2 ~ thr_L1 / 1.4` as a start.

**A cheap integer sqrt is available if you ever want true magnitude in reported
units** — 5-iteration Newton or a 16-step restoring-shift `isqrt32` is ~40
cycles. The justification for using one is narrow but real: run it **once per
event on the peak only** (400 times per second would be waste; 4 times per
second is nothing). Do **not** put it in the per-sample path.

**Rejected: per-axis thresholding as the primary decision.** Badge orientation
on a lanyard is arbitrary, so any fixed per-axis rule either misses knocks on
one face or fires on gravity-axis footfalls.

**Rejected, and worth saying why because it looks clever: projecting the AC
vector onto the known gravity direction.** `g_ema` tells you which way is down,
so you could veto purely-vertical events as footfalls. It costs 3 MULs and a
divide, and it **rejects a legitimate knock to a badge lying face-up on a
desk**, which is exactly along gravity. It trades a core use case for a marginal
veto. Don't.

**Kept as metadata, free:** `argmax(abs(ac_x),abs(ac_y),abs(ac_z))` at the peak
sample is the `axis` hint in the event struct (§4). Z-dominant ⇒ face or
flat-on-desk knock; X/Y-dominant ⇒ edge knock. Costs one compare already in the
loop.

### 1.6 Ring buffer — 128 entries, and why the detector needs none

**The detector itself is buffer-free.** All of its state is six scalars:
`ema[3]`, `qrun`, `agit`, and the in-window accumulators. The isolation test is
a *leaky counter*, not a windowed max over history (§2.2), and the decay test is
forward-looking state, not a backward buffer read. That is deliberate — it keeps
the hot path O(1) with no modular indexing and no amortized-O(1) monotonic deque
to get wrong at 2 a.m.

The ring exists **only for offline analysis**, which is where it earns its keep:
capture 128 samples around each detection, dump as CSV, and re-tune on a laptop
against real data instead of guessing on the bench.

- **128 entries** (power of two → `& (N-1)` masking), `int16_t[4]` each
  (x, y, z, mag) = **1024 bytes**. Free against ~400 KB, no PSRAM needed.
- At a 2 ms loop that is **256 ms of context** — covers the 200 ms quiet window
  plus the event plus the decay window.
- Bump to **256 entries (2 KB)** for a full second of pre-roll at 200 Hz. Still
  free.

### 1.7 Bench check for the anti-alias claim

The "attenuated, not aliased" claim in §1.1 is **[U]**. Cheap falsification:
knock the badge 20 times at constant moderate force and look at the peak spread.
If the internal filter is doing its job, peaks cluster within ~±25%. **If peaks
scatter 2–3x on visually identical knocks, you are seeing alias-driven phase
sensitivity** — the detector will feel random no matter how you tune it. The fix
is to drop to 200 Hz (narrower internal BW, more attenuation) and widen the
windows, which is the opposite of intuition and worth knowing before you burn an
hour.

### 1.8 Fallbacks, decided in advance

| If the bench shows | Do this |
|---|---|
| Flip test reads ±1000 — FS write didn't take | Stay at ±2 g / 1 mg. Set `thr` to **1600 counts** (just under the 2047 rail) and lean entirely on isolation + ringdown, because amplitude is compressed. Expect degraded slam rejection and **document it** rather than chasing it. |
| `hz_now` sags below 380 under display load | Drop to 200 Hz / 4 ms poll / K=6; halve all iteration counters (§2.7 gives both columns). |
| `i2c_err` climbs during NFC scans | The pause/resume contract (§4) isn't wired. Fix that; do not raise the timeout. |
| Knock peaks scatter 2–3x | See §1.7 — drop ODR, widen windows. |

### 1.9 Stretch: the FIFO (biggest available robustness win, unverified)

LIS3DH-compatible parts carry a **32-level FIFO** (`CTRL_REG5[6]` = FIFO_EN,
`FIFO_CTRL_REG 0x2E` mode, `FIFO_SRC 0x2F` sample count). If the SC7A20
implements it, stream mode lets you poll at **20 ms** and drain up to 32 samples
per read. That would make the detector **structurally immune to display-blit
jitter** (the single biggest threat on one core), cut I2C transactions ~10x, and
remove the duplicate-sample problem entirely.

It is **not documented in the badge spec**, so: build the polled path first (it
definitely works), then if time remains probe at runtime — write
`FIFO_CTRL_REG = 0x40` (stream mode), set `CTRL_REG5 |= 0x40`, wait 100 ms, read
`FIFO_SRC`; if the FSS count field is non-zero and increments, the FIFO is real.
**Gate the whole feature on that runtime probe and keep polled-single as the
fallback.** Do not refactor the detector for it — only the sample source
changes.

---

## 2. Discriminators — pressure-testing the proposed pair

The proposal was: (a) ISOLATION — the window before the spike must be quiet,
which kills walking and swinging; (b) DECAY — the signal must fall under a
fraction of threshold within tens of ms, which kills sustained pushes.

**Verdict: (a) is necessary but is credited with work it cannot do, and (b) is
defending against a weak attacker and should be reformulated into something
stronger. Neither is the primary discriminator.** Detail below.

### 2.1 Where ISOLATION fails

**Failure 1 — it does nothing against a neighbour's table slam.** This is the
big one, and it is the exact scenario named as a threat. A slam arriving at a
badge lying on the desk is a **single isolated impulse preceded by perfect
quiet** — byte-for-byte the signature of a deliberate knock. Isolation has no
purchase here whatsoever. Something else has to carry this case (§2.4).

**Failure 2 — isolation cannot actually kill walking, and the arithmetic shows
it.** Walking cadence is 1.8–2.2 Hz, so step period is 450–550 ms, and the
footfall impulse itself is ~100 ms wide. **The quiet gap between footfalls is
therefore ~350–450 ms.** Any required quiet window *shorter* than that gap is
satisfied by a walker on every stride. To beat walking by isolation alone you
would need a ~450 ms quiet requirement, which badly hurts the "knock it on a
lanyard" use case. **What actually kills walking is amplitude** (footfall at the
chest is 125–300 counts, well under a 375-count threshold). Isolation's real job
is narrower and should be stated honestly: *catch the rare hard footfall —
stomping, stairs, jumping — by noticing it arrives inside continuous motion.*

**Failure 3 — it fights the requirement directly.** "A missed bump is a failed
interaction," and the stated gestures include "on a lanyard." If the user walks
up and knocks while still moving, the pre-window is not quiet and isolation
rejects a real gesture. This is why the quiet gate must be set generously (well
above the noise floor, near walking amplitude) and why the counter must be
**leaky** rather than hard-reset (§2.2).

**Failure 4 — sitting down passes it.** Stand still (quiet) → descend (low
frequency, often under the quiet gate) → land (impulse). That sequence satisfies
isolation. A sit-down jolt also *decays fast*, so it satisfies the decay test
too. **Sitting down is the hardest false positive in the set and neither of the
two proposed tests reliably stops it.** Only amplitude plus the agitation veto
(§2.5) touch it.

**Keep isolation anyway** — it is 3 lines, it genuinely handles knock-during-
continuous-motion, and it is the cheapest veto available. Just do not budget it
for walking or slams.

### 2.2 ISOLATION, as actually specified (leaky quiet credit)

Naive version: `quiet_run` counts consecutive samples below `qgate`, reset to 0
on any violation. **Flaw:** one noise blip 150 ms before a real knock destroys
all credit and rejects the gesture. On a lanyard that blip is routine.

Specified version — a **leaky** counter:

```c
if (mag < qgate) { if (qrun < 8192) qrun++; }
else             { qrun -= qpen; if (qrun < 0) qrun = 0; }
```

A single isolated blip costs `qpen` samples of credit (20 ms at `qpen=8`), not
all of it. A sustained disturbance still drives credit to zero fast: a 100 ms
footfall (50 iterations above the gate) costs 400 samples of credit, far more
than the 100 required. **Three lines, strictly more robust than the hard reset,
and it is what makes isolation survivable for the lanyard case.**

- `qgate` = **60 counts (240 mg)** **[S]** — above standing-still/handheld
  jitter (5–13 counts) and lanyard swing's low end, below walking (125–300).
  Expect to tune to 75–100 if in-hand knocks while standing get rejected.
- `qneed` = **200 ms** (100 iterations at 2 ms) **[S]** — deliberately *shorter*
  than the 350 ms footfall gap, because (per §2.1 failure 2) trying to beat
  walking here is a losing trade against the lanyard use case. Walking is
  handled by amplitude and agitation.
- `qpen` = **8** **[S]** — rarely touched; it sets how expensive a blip is.

### 2.3 Where DECAY fails

**Failure 1 — it defends against a weak attacker.** Squeezing a badge produces
well under 500 mg, applied slowly. At a 375-count (1500 mg) threshold a squeeze
**never crosses the trigger in the first place**, so the decay test is never
consulted. The test is nearly free, so keep it, but it is not carrying the weight
the proposal assigns it.

**Failure 2 — venue bass is not a decay problem.** Bass is continuous, so it
raises the baseline and is caught by the *quiet gate*, not by decay. And at
40–60 Hz the amplitude at a badge not touching a speaker cabinet is a few tens
of mg — a non-threat against a 1500 mg threshold. Expect test J in §6 to pass
trivially; if it fails, the threshold is far too low, not the decay window.

**Failure 3 — strict decay rejects real knocks on a lanyard.** After a knock the
badge swings and **bounces off the wearer's chest**, producing a secondary
impulse 20–60 ms later. A rule of the form "must stay below `dgate` for the whole
window" rejects that legitimate gesture. The test must be a *tolerant* one.

**Failure 4 — decay cannot distinguish a slam either.** A sit-down jolt and a
transmitted slam both decay quickly in amplitude. "Falls below a fraction of
threshold within tens of ms" is true of almost every impulsive event. As stated,
decay separates *impulses from sustained forces*, which is a much narrower job
than separating *knocks from other impulses*.

### 2.4 The replacement that carries the weight: the RINGDOWN test

**The physics that actually separates a direct knock from a transmitted table
slam:**

- **Direct knock.** The impulse is applied *to the badge*. A ~30 g object takes
  a large velocity change, then it is free (hand / lanyard / desk contact) and
  the energy is dumped into heavily damped soft tissue or a massive desk. Result:
  **high peak, one dominant lobe, dies in 10–40 ms.**
- **Neighbour's slam, badge on the desk.** The impulse is applied *to the desk*.
  The desk top is a large-mass, low-frequency plate (bending modes ~20–100 Hz)
  with low damping, and it **rings**. The badge rides along. Result:
  **lower peak, oscillatory, several cycles, 50–200 ms of ringdown.**

So the separator is not "does it decay" — both do — it is **"how many times does
it come back up."** Measure it with one counter:

> **RINGDOWN:** over the `dwin` iterations after the trigger, count the samples
> where `mag >= dgate`. Pass only if that count is <= `dhot_max`, **and** the
> window ends with at least `dq_need` consecutive samples below `dgate`.

Why this is a strict improvement on the proposed decay test:

- A direct knock plunges: few hot samples. ✅
- A desk ringdown keeps re-crossing: many hot samples. ✅ **(the slam case that
  isolation and plain decay both miss)**
- A lanyard chest-bounce produces 1–2 extra hot samples, inside the allowance. ✅
  **(fixes §2.3 failure 3 — the real knock survives)**
- A sustained push or squeeze is hot for the entire window. ✅ (keeps everything
  the original decay test bought)

It is the same cost as the proposed test — one counter and one compare — and it
strictly dominates it. **This is the recommended replacement.**

Numbers:

- `dgate` = `thr >> dshift`, `dshift` = **2** → **thr/4 = 94 counts (375 mg)**.
  The 1/4 ratio is grounded in damping: a hand-held badge has zeta > 0.2, so a
  1500 mg peak e-folds below 375 mg within 2–3 cycles of a 100–300 Hz mode =
  well under 60 ms. **[P ratio, S exact value]** Tunable 1..3 (1/2 .. 1/8).
- `dwin` = **60 ms** (30 iterations at 2 ms) **[P]** — hand/lanyard ringdown
  dies in 10–40 ms; desk-coupled ringdown lasts 50–200 ms. 60 ms sits **in the
  gap**: long enough that a slam is still visibly hot, short enough that a real
  knock is already dead.
- `dhot_max` = **10 of 30 (33%)** **[S]** — the main slam-rejection knob; §6
  sweeps it 10 → 6 → 4.
- `dq_need` = **5 consecutive** (10 ms) **[S]** — "it actually went quiet,"
  not "it happened to be low on the last sample."

**Latency budget, and why `dwin` is not 150 ms:** the decision lands at
peak + 60 ms, so gesture-to-LED latency is ~65 ms. That is under the ~100 ms
threshold where an interaction still feels instantaneous. **[P]** A 150 ms window
would discriminate slams slightly better and feel noticeably laggy. 60 ms is the
right side of that trade.

### 2.5 The discriminator the proposal is missing: AGITATION (train veto)

Per §2.1 failure 2, isolation's horizon is structurally too short to see
**cadence**. Walking, running, repeated table drumming and being jostled in a
crowd are all *repetitive*; a deliberate knock is *singular*. That is a 1–2
second observation and isolation cannot make it.

One more leaky integrator, a ~2 s horizon:

```c
if (mag < qgate) { if (agit) agit--; }
else             { agit += 8; if (agit > 8192) agit = 8192; }
```

Reject a trigger when `agit > agit_max`. Walking at 2 Hz gives 4 footfalls in
2 s and keeps `agit` pinned high; a single knock after standing still barely
moves it.

- `agit_max` = **600** **[S]**, `agit_en` = **1** **[S]**

**The honest cost:** this requires the user to be roughly still for ~1–2 s
before knocking. That is natural behaviour (you stop to interact) and the stated
threat model treats lanyard-while-walking as a *false positive* scenario rather
than a gesture scenario. But it is a real constraint, so **it is exposed as a
console toggle (`a0`/`a1`)** — measure the trade on the bench in §6 step 5
instead of guessing. If it costs too many true positives, ship with `a0` and
accept more walking FPs.

### 2.6 Refractory — and the second lockout nobody remembers

**Refractory = 250 ms** (125 iterations at 2 ms). **[P]** Four constraints pin
this number:

1. **Longer than any ringdown.** Desk/hand ringdown is under 100 ms, so 250 ms
   gives 2.5x margin — the physical event is definitively over.
2. **Longer than a human double-knock.** The fastest a person can deliberately
   knock twice is **~100–150 ms**. Anything below that and an eager user's
   double-knock fires two events, violating "exactly one event."
3. **Short enough to retry.** A user who knocks, sees nothing, and knocks again
   does so at ~400 ms. 250 ms lets the retry through.
4. **Matches perceptual grouping.** ~250 ms is roughly the window inside which a
   person reads two impulses as "one action."

**The lockout nobody remembers: a separate, shorter `rlock` after a *rejected*
candidate — 100 ms (50 iterations) [S].** Without it, a desk ringdown that pokes
above `thr` five times in 150 ms runs **five full decay evaluations**, burning
CPU and giving five independent chances for one to pass by luck. With it, the
ringdown is evaluated once. This is a small detail with a large effect on
measured false-positive rate.

**And: force `qrun = 0` when the lockout expires.** Otherwise stale quiet credit
accumulated before the event lets the next trigger skip its isolation test.

### 2.7 Final parameter table (both ODR configurations)

Console units are **milliseconds** (what humans think in); internals are
iteration counts via `MS2N(ms) = (ms)*LOOP_HZ/1000`.

| Param | Console | 400 Hz ODR / 2 ms loop | 200 Hz ODR / 4 ms loop | Grounding |
|---|---|---|---|---|
| EMA shift `K` | — | 7 (tau 256 ms) | 6 (tau 256 ms) | **[P]** |
| Warm-up blank | — | 320 it (512 ms) | 128 it (512 ms) | **[P]** 2*tau |
| `thr` peak threshold | `t` | **375 counts (1500 mg)** | same | **[S]**, bracketed **[P]** |
| `qgate` quiet gate | `q` | **60 counts (240 mg)** | same | **[S]**, bracketed **[P]** |
| `qneed` quiet credit | `n` | **100 it (200 ms)** | 50 it (200 ms) | **[S]** |
| `qpen` blip penalty | `p` | **8** | 8 | **[S]** |
| `dshift` decay ratio | `d` | **2 (thr/4 = 94)** | 2 | ratio **[P]** |
| `dwin` decay window | `w` | **30 it (60 ms)** | 15 it (60 ms) | **[P]** |
| `dhot_max` ringdown allow | `h` | **10 (33%)** | 5 (33%) | **[S]** |
| `dq_need` quiet confirm | — | **5 it (10 ms)** | 3 it (12 ms) | **[S]** |
| `refr` refractory | `r` | **125 it (250 ms)** | 63 it (252 ms) | **[P]** |
| `rlock` reject lockout | — | **50 it (100 ms)** | 25 it (100 ms) | **[S]** |
| `agit_max` | `a` | **600** | 300 | **[S]** |

**Which numbers are physically grounded vs. starting points, summarised:**

- **Grounded [P]:** ODR 400 Hz, ±8 g full scale, EMA shift / 256 ms tau, 512 ms
  warm-up, 60 ms decay window, 250 ms refractory, the thr/4 decay ratio, and
  every amplitude *bracket* in the §1.2 table.
- **Starting points [S]:** the absolute `thr` and `qgate` values, `qneed`,
  `qpen`, `dhot_max`, `dq_need`, `rlock`, `agit_max`. These are what §6 tunes.
- **Unverified [U]:** SC7A20 FS-bit behaviour (§1.3 flip test), the internal
  anti-alias filter (§1.7 spread test), the FIFO (§1.9 probe).

---

## 3. Working code

### 3.1 Required sdkconfig

```
CONFIG_FREERTOS_HZ=1000                  # MANDATORY — see §0 finding 1
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y     # badge console is native USB
```

### 3.2 `bump_detect.c`

```c
/* bump_detect.c — SC7A20 deliberate-knock detector, HTN 2026 badge.
 * Fixed point only: no float, no sqrt, no division, no dynamic alloc.
 * O(1) per sample. Detector state is 6 scalars; the ring is debug-only. */
#include "bump_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_timer.h"

#define REG_CTRL1   0x20
#define REG_CTRL4   0x23
#define REG_OUT_X_L 0x28
#define I2C_AUTOINC 0x80            /* set bit7 of reg addr for burst read   */
#define EMA_SHIFT   7               /* tau = 2^7/500Hz = 256 ms (f_c 0.62Hz) */
#define EMA_FRAC    8               /* MUST be >= EMA_SHIFT or the EMA stalls*/
#define POLL_MS     2               /* 500 Hz loop vs 400 Hz ODR             */
#define WARM_N      320             /* 512 ms = 2*tau, detection blanked     */
#define CLIP_CNT    2040            /* per-axis rail at 12-bit              */
#define RING_N      128u            /* power of two                          */

bump_cfg_t   g_bcfg = BUMP_CFG_DEFAULTS;   /* word-sized fields: lock-free  */
bump_stats_t g_bst;
int16_t      g_bring[RING_N][4];           /* x,y,z,mag — CSV only          */
uint32_t     g_bring_w;

static QueueHandle_t           s_q;
static i2c_master_dev_handle_t s_dev;
static volatile bool           s_pause;

static inline int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static bool accel_read(int16_t o[3], int *clip)
{
    uint8_t reg = REG_OUT_X_L | I2C_AUTOINC, b[6];
    /* BOUNDED: NFC shares this bus and can wedge it. Never wait forever. */
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 6, pdMS_TO_TICKS(5)) != ESP_OK)
        return false;
    for (int i = 0; i < 3; i++) {
        o[i] = (int16_t)(b[2*i] | (b[2*i+1] << 8)) >> 4;   /* 12-bit left-just. */
        if (o[i] >= CLIP_CNT || o[i] <= -CLIP_CNT) *clip = 1;
    }
    return true;
}

static void bump_task(void *arg)
{
    int32_t ema[3] = {0,0,0}, qrun = 0, agit = 0;
    int32_t warm = WARM_N, lock = 0, dn = 0, peak = 0, hot = 0, qcons = 0;
    int     pk_ax = 0, in_decay = 0, clip = 0, pk_clip = 0;
    int16_t raw[3], prev[3] = {0,0,0};
    int64_t t_prev = esp_timer_get_time();
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        if (s_pause) {                     /* NFC owns the bus */
            vTaskDelay(pdMS_TO_TICKS(10));
            next = xTaskGetTickCount();    /* else DelayUntil replays backlog */
            warm = WARM_N; in_decay = 0; lock = 0; qrun = 0;  /* re-seed+re-earn */
            continue;
        }
        xTaskDelayUntil(&next, pdMS_TO_TICKS(POLL_MS));
        clip = 0;
        if (!accel_read(raw, &clip)) { g_bst.i2c_err++; continue; }

        /* ---- health: prove we are not being starved by display/LEDs ---- */
        int64_t  now = esp_timer_get_time();
        uint32_t gap = (uint32_t)(now - t_prev); t_prev = now;
        if (gap > g_bst.worst_gap_us) g_bst.worst_gap_us = gap;
        if (gap > (uint32_t)POLL_MS * 2000u) g_bst.late++;
        if (raw[0]==prev[0] && raw[1]==prev[1] && raw[2]==prev[2]) g_bst.dup++;
        prev[0]=raw[0]; prev[1]=raw[1]; prev[2]=raw[2];
        g_bst.samples++;

        /* ---- gravity removal + L1 magnitude + axis hint ---- */
        int32_t mag = 0, amax = 0; int ax = 0;
        for (int i = 0; i < 3; i++) {
            int32_t xq = (int32_t)raw[i] << EMA_FRAC;
            if (warm == WARM_N) ema[i] = xq;               /* seed on 1st sample */
            ema[i] += (xq - ema[i]) >> EMA_SHIFT;
            int32_t ac = iabs32((int32_t)raw[i] - (ema[i] >> EMA_FRAC));
            mag += ac;                                      /* L1 norm */
            if (ac > amax) { amax = ac; ax = i; }
        }
        uint32_t w = g_bring_w++ & (RING_N - 1);
        g_bring[w][0]=raw[0]; g_bring[w][1]=raw[1]; g_bring[w][2]=raw[2];
        g_bring[w][3]=(int16_t)(mag > 32767 ? 32767 : mag);
        if (mag > g_bst.mag_max) g_bst.mag_max = mag;
        g_bst.mag_now = mag; g_bst.qrun = qrun; g_bst.agit = agit;

        /* ---- two leaky integrators: short-horizon quiet, long-horizon agitation ---- */
        if (mag < g_bcfg.qgate) { if (qrun < 8192) qrun++; if (agit) agit--; }
        else { qrun -= g_bcfg.qpen; if (qrun < 0) qrun = 0;
               agit += 8; if (agit > 8192) agit = 8192; }

        if (warm) { warm--; continue; }
        if (lock) { if (--lock == 0) qrun = 0; continue; }  /* refractory/lockout */

        if (!in_decay) {                                   /* ---- ARMED ---- */
            if (mag < g_bcfg.thr) continue;
            if (qrun < g_bcfg.qneed) {                     /* ISOLATION veto */
                g_bst.rej_quiet++; lock = g_bcfg.rlock_n; continue; }
            if (g_bcfg.agit_en && agit > g_bcfg.agit_max) { /* AGITATION veto */
                g_bst.rej_agit++;  lock = g_bcfg.rlock_n; continue; }
            peak = mag; pk_ax = ax; pk_clip = clip;
            hot = 0; qcons = 0; dn = g_bcfg.dwin; in_decay = 1;
            continue;
        }

        /* ---- RINGDOWN window ---- */
        if (mag > peak) { peak = mag; pk_ax = ax; }
        if (clip) pk_clip = 1;
        if (mag >= (g_bcfg.thr >> g_bcfg.dshift)) { hot++; qcons = 0; }
        else                                      { qcons++; }
        if (--dn > 0) continue;

        in_decay = 0;
        lock = g_bcfg.refr_n;                              /* refractory starts */
        if (hot > g_bcfg.dhot_max || qcons < g_bcfg.dq_need) {
            g_bst.rej_decay++; continue; }

        bump_evt_t e = {
            .t_us  = now,
            .peak  = (uint16_t)(peak > 65535 ? 65535 : peak),
            .axis  = (uint8_t)pk_ax,
            .flags = (uint8_t)((pk_clip ? BUMP_F_CLIPPED : 0) |
                     (peak < g_bcfg.thr + (g_bcfg.thr >> 1) ? BUMP_F_WEAK : 0) |
                     (g_bcfg.agit_en ? 0 : BUMP_F_UNVETTED)),
            .seq   = ++g_bst.events,       /* counts attempts: gaps reveal drops */
        };
        if (xQueueSend(s_q, &e, 0) != pdTRUE) g_bst.q_drop++;  /* drop-newest */
    }
}

esp_err_t bump_detect_init(const bump_detect_init_t *ic)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address  = ic->addr ? ic->addr : 0x19,
                               .scl_speed_hz    = 400000 };
    /* We ADD a device to the caller's bus. We never create or delete the bus. */
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(ic->bus, &dc, &s_dev), "bump", "add");
    uint8_t c1[2] = { REG_CTRL1, 0x77 };   /* 400 Hz, XYZ on                  */
    uint8_t c4[2] = { REG_CTRL4, 0xA0 };   /* BDU, little-endian, +/-8 g      */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, c1, 2, pdMS_TO_TICKS(20)), "bump","c1");
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, c4, 2, pdMS_TO_TICKS(20)), "bump","c4");
    s_q = xQueueCreate(BUMP_Q_DEPTH, sizeof(bump_evt_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    *ic->out_queue = s_q;
    return xTaskCreate(bump_task, "bump", 3072, NULL,
                       ic->task_prio ? ic->task_prio : 6, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

void bump_detect_pause(void)  { s_pause = true;  }
void bump_detect_resume(void) { s_pause = false; }
```

### 3.3 Notes on the code

- **Task period 2 ms, priority 6.** Above the UI/display task (give that 4 or 5),
  below the I2C driver and `esp_timer` internals. No core affinity — there is
  only one core. Stack 3072 B; **no `printf` in this task** (printing lives in
  the console task, §5), which is what keeps the stack this small.
- **No `STATUS`/ZYXDA read.** Polling at 500 Hz against a 400 Hz ODR means ~20%
  of reads return the previous sample. With BDU set those duplicates are
  harmless — a repeated value is a flat region, which only ever helps the quiet
  and ringdown tests — and skipping `STATUS` halves bus traffic from ~13 to ~9
  byte-times (~290 µs → ~200 µs per sample, 10% CPU instead of 17.5%). The
  `dup` counter exists so you can *prove* the ODR is real rather than assume it.
- **Bounded 5 ms I2C timeout, every transaction.** Directly per the badge
  spec's warning that the MFRC522 can wedge the bus on droopy battery power. A
  failure increments `i2c_err` and the loop continues; it never blocks the badge.
- **`next = xTaskGetTickCount()` on resume is load-bearing.** `xTaskDelayUntil`
  keeps an absolute deadline; without the reset it would try to "catch up" on
  every missed 2 ms period after a 750 ms NFC scan and hammer the bus with
  hundreds of back-to-back reads.
- **`g_bcfg` is a lock-free global.** All fields are word-sized, so writes are
  atomic on RV32. A deliberate trade: a mutex around the config would be correct
  and would also add a lock to the 500 Hz hot path for no practical benefit when
  the only writer is a human typing one character at a time.
- Detector task body is ~75 lines; with init and the header it is ~110.

---

## 4. Integration contract — `bump_detect.h`

```c
/* bump_detect.h — integration contract for the HTN 2026 badge knock detector.
 *
 * OWNERSHIP RULES, read these first:
 *  1. This module does NOT own the I2C bus. You create the bus (shared with the
 *     MFRC522 NFC reader at 0x26); we only i2c_master_bus_add_device() onto it.
 *     We never call i2c_new_master_bus() or i2c_del_master_bus().
 *  2. Before any NFC transaction, call bump_detect_pause(). After, resume().
 *     While paused we touch neither the bus nor the queue.
 *  3. resume() re-seeds the gravity estimate and blanks detection for 512 ms.
 *     That is intentional: without it, the gravity step across a scan gap fires
 *     a false bump on every NFC read.
 *  4. The queue is ours to write and yours to read. Do not delete it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"

/* ---------------- event ---------------- */

typedef enum { BUMP_AXIS_X = 0, BUMP_AXIS_Y = 1, BUMP_AXIS_Z = 2 } bump_axis_t;

/* Peak hit the +/-8g rail: the gesture was harder than we can measure, so
 * `peak` is a floor, not a value. Still a valid bump. */
#define BUMP_F_CLIPPED   0x01
/* Peak was under 1.5x threshold — a marginal bump. Consumers that want to be
 * conservative (e.g. a destructive action) may choose to ignore these. */
#define BUMP_F_WEAK      0x02
/* The agitation veto was disabled when this fired, so it was not screened for
 * repetitive motion. Lower confidence. */
#define BUMP_F_UNVETTED  0x04

typedef struct {
    int64_t  t_us;    /* esp_timer_get_time() at the decision sample.
                       * NOTE: this is peak + dwin (~60 ms), not the instant of
                       * impact. Subtract ~60 ms if you need impact time. */
    uint16_t peak;    /* peak L1 magnitude, counts. 1 count = 4 mg at +/-8 g. */
    uint8_t  axis;    /* bump_axis_t of the dominant axis at the peak sample.
                       * Z => face or flat-on-desk knock; X/Y => edge knock.
                       * A HINT, not a guarantee. */
    uint8_t  flags;   /* BUMP_F_* bitmask */
    uint32_t seq;     /* monotonic, counts DETECTIONS not deliveries. A gap in
                       * seq on the consumer side means the queue overflowed. */
} bump_evt_t;         /* 16 bytes */

/* ---------------- queue ----------------
 * DEPTH 4. OVERFLOW POLICY: drop-newest, non-blocking, counted in
 * bump_stats_t.q_drop.
 *
 * Why 4: at a 250 ms refractory, 4 events is one full second of backlog. A
 * consumer that cannot drain one second of bumps is wedged, and the right
 * answer is to drop rather than to deliver stale events the user already gave
 * up on and knocked again for.
 *
 * Why drop-NEWEST rather than drop-oldest: if the consumer is briefly blocked
 * (a full-screen ST7789 blit), the OLDEST queued event is the one the user is
 * still waiting for feedback on. It is also a single non-blocking xQueueSend
 * with no read-modify-write race against the consumer.
 *
 * Consumer contract: drain with a timeout, not xQueueReceive(portMAX_DELAY),
 * and treat every event as "fire feedback now". Check seq for gaps if you care.
 */
#define BUMP_Q_DEPTH 4

/* ---------------- config (live-tunable, see §5) ---------------- */

typedef struct {
    int32_t thr;        /* peak threshold, counts (375 = 1500 mg)            */
    int32_t qgate;      /* quiet gate, counts (60 = 240 mg)                  */
    int32_t qneed;      /* quiet credit required, loop iterations (100=200ms)*/
    int32_t qpen;       /* credit penalty per noisy sample (8)               */
    int32_t dshift;     /* decay gate = thr >> dshift (2 => thr/4)           */
    int32_t dwin;       /* ringdown window, iterations (30 = 60 ms)          */
    int32_t dhot_max;   /* max samples above decay gate in window (10 of 30) */
    int32_t dq_need;    /* consecutive quiet samples to confirm (5 = 10 ms)  */
    int32_t refr_n;     /* refractory, iterations (125 = 250 ms)             */
    int32_t rlock_n;    /* post-reject lockout, iterations (50 = 100 ms)     */
    int32_t agit_max;   /* agitation veto level (600)                        */
    int32_t agit_en;    /* 1 = agitation veto on                             */
} bump_cfg_t;

#define BUMP_CFG_DEFAULTS { .thr=375, .qgate=60, .qneed=100, .qpen=8,       \
                            .dshift=2, .dwin=30, .dhot_max=10, .dq_need=5, \
                            .refr_n=125, .rlock_n=50,                      \
                            .agit_max=600, .agit_en=1 }

/* ---------------- health / stats ---------------- */

typedef struct {
    uint32_t samples;       /* total samples read                            */
    uint32_t dup;           /* identical consecutive raws: proves ODR is real*/
    uint32_t late;          /* inter-sample gaps > 2x poll period            */
    uint32_t worst_gap_us;  /* worst gap: display starvation shows up HERE    */
    uint32_t i2c_err;       /* bounded-timeout failures (NFC bus contention)  */
    uint32_t events;        /* detections (== last seq)                       */
    uint32_t q_drop;        /* events dropped: consumer too slow              */
    uint32_t rej_quiet;     /* vetoed by isolation                            */
    uint32_t rej_agit;      /* vetoed by agitation                            */
    uint32_t rej_decay;     /* vetoed by ringdown                             */
    int32_t  mag_now, mag_max, qrun, agit;   /* live values for the console   */
} bump_stats_t;

extern bump_cfg_t   g_bcfg;    /* lock-free; word-sized fields only */
extern bump_stats_t g_bst;
extern int16_t      g_bring[128][4];   /* x,y,z,mag ring — CSV dump only */
extern uint32_t     g_bring_w;

/* ---------------- lifecycle ---------------- */

typedef struct {
    i2c_master_bus_handle_t bus;         /* REQUIRED. Yours. Shared with NFC. */
    uint8_t                 addr;        /* 0 => 0x19                          */
    QueueHandle_t          *out_queue;   /* REQUIRED. We create, you receive.  */
    int                     task_prio;   /* 0 => 6. Put your UI task below it. */
} bump_detect_init_t;

esp_err_t bump_detect_init(const bump_detect_init_t *ic);

/* Call around every NFC transaction. Idempotent, safe from any task.
 * resume() re-seeds gravity and blanks detection for 512 ms. */
void bump_detect_pause(void);
void bump_detect_resume(void);
```

### 4.1 Caller skeleton

```c
QueueHandle_t bumpq;
bump_detect_init(&(bump_detect_init_t){ .bus = my_shared_bus,
                                        .out_queue = &bumpq,
                                        .task_prio = 6 });
/* UI task */
bump_evt_t e;
if (xQueueReceive(bumpq, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (!(e.flags & BUMP_F_WEAK)) flash_leds_dim();
}
/* NFC scan */
bump_detect_pause();  nfc_scan_once();  bump_detect_resume();
```

**Double-tap escape hatch:** `t_us` plus `seq` give the consumer everything
needed to build a double-tap in *application* code later, with zero firmware
change. See §7 for why it should not go in the firmware.

---

## 5. Runtime tuning over the USB console

**Why this is the highest-leverage item:** every reflash costs a manual
download-mode hold (hold Start while plugging USB). A tuning loop that requires
a reflash per parameter change cannot fit in 45 minutes. A tuning loop over the
console fits in 45 minutes with time to spare.

**Console mechanics for this badge, specifically:**

- It is **USB-Serial-JTAG**, so install the VFS driver
  (`esp_vfs_usb_serial_jtag_use_driver()`) or read via
  `usb_serial_jtag_read_bytes()`. `idf.py monitor` does **not** echo — echo
  typed characters yourself or tuning feels broken.
- **Line ending is a bare `\r`.** Terminate the parser on `\r`; a `\n`-only
  parser will appear completely dead.
- **Keep every write under 256 bytes** (USB-Serial-JTAG RX ring). The stats line
  below is ~140 bytes. CSV output is batched to ~8 lines (~256 B) per write.
- Run the parser in its **own task at priority 3** (below the detector). The
  detector must never block on console I/O.

### 5.1 Command set

Grammar: `<key>\r` reads, `<key><number>\r` writes. One character, no prompt.

| Key | Writes | Reads | Units |
|---|---|---|---|
| `t` | peak threshold | current | counts (375 = 1500 mg) |
| `q` | quiet gate | current | counts (60 = 240 mg) |
| `n` | quiet credit required | current | **ms** (200) |
| `r` | refractory | current | **ms** (250) |
| `d` | decay shift (1..3) | current | shift → thr/2, thr/4, thr/8 |
| `w` | decay/ringdown window | current | **ms** (60) |
| `h` | max hot samples in window | current | count (10 of 30) |
| `p` | blip penalty | current | samples (8) |
| `a` | `a0`/`a1` veto off/on; `aNNN` sets level | current | level (600) |
| `s` | — | **one-line stats + full config** | see §5.2 |
| `l` | toggle live telemetry at 5 Hz | — | mag / qrun / agit |
| `c` | dump the 128-sample ring as CSV | — | post-event forensics |
| `C` | toggle continuous raw CSV stream | — | bulk capture |
| `z` | zero all stats, re-seed EMA, re-blank | — | start of every test |
| `x` | simulate pause/resume (NFC rehearsal) | — | tests the resume path |
| `S` | **save config to NVS** | — | **mandatory, see below** |
| `L` | load config from NVS | — | — |
| `D` | restore compiled defaults (no save) | — | escape from bad tuning |
| `?` | — | command help | keep under 256 B, 2 writes |

**`S`/`L` are the two most important commands in the table.** Without NVS
persistence, a power cycle — or a brown-out from running the 6 WS2812Bs too
bright on AA cells, which this board does — silently reverts every tuned value
and you will tune twice. Load NVS config at boot; fall back to
`BUMP_CFG_DEFAULTS` if the blob is missing or the version tag mismatches.

### 5.2 The stats line (the starvation proof)

One line, ~140 bytes, CSV-ish so it is greppable and pasteable:

```
ST hz=398 dup=97 late=0 gap=3100 i2e=0 ev=7 qd=0 rq=2 ra=0 rd=5 mag=11 pkmax=742 qr=1022 ag=0 | t=375 q=60 n=200 r=250 d=2 w=60 h=10 p=8 a=1
```

**The four fields that prove the detector is not being starved by the display or
the LEDs:**

| Field | Meaning | Pass |
|---|---|---|
| `hz` | samples/sec over the last 1 s window | **>= 380** (95% of 400) |
| `late` | gaps > 2x poll period (> 4 ms) | **<= 5 per minute** |
| `gap` | worst inter-sample gap, µs | **<= 15000** (15 ms) |
| `dup` | identical consecutive raw triples | **~20% of `samples`** |

`hz`, `late` and `gap` catch starvation. **`dup` catches the subtler failure:**
if `dup` is near zero, you are sampling *slower* than the ODR and silently
losing samples; if `dup` is near 100%, the sensor is **not actually producing
data at 400 Hz** and your ODR write did not take — you are detecting on stale
registers. Expecting ~20% (500 Hz poll / 400 Hz ODR) turns "the ODR is 400 Hz"
from an assumption into a measurement.

A full-screen 320x240 RGB565 blit at 40 MHz SPI is ~31 ms of transfer. If that
is done synchronously it will show up immediately as `gap` ~ 31000 and `late`
climbing. **That is the number that tells the UI author to use DMA stripes
(per the badge spec's two 30-row buffers) rather than a blocking full blit.**

### 5.3 CSV formats

```
R,<t_us>,<x>,<y>,<z>,<mag>        # raw sample  (~32 B/line)
E,<t_us>,<peak>,<axis>,<flags>    # event marker, interleaved
```

- **`c` (triggered ring dump) is the one to use.** 128 lines ~ 4 KB, batched 8
  lines per write. Low overhead, and it captures exactly the window you care
  about.
- **`C` (continuous) is for bulk capture only, and it perturbs what you are
  measuring.** 400 Hz x 32 B = 12.8 KB/s. USB-Serial-JTAG handles the
  bandwidth, but the formatting and write calls add jitter — watch `late` climb
  while it runs. Prefer `c`. If you must use `C`, re-check acceptance numbers
  with it off.
- Capture to a file with `idf.py monitor` output redirection or
  `cat /dev/ttyACM0 > cap.csv` (only one process may own the port at a time —
  close the monitor first).

---

## 6. Acceptance test protocol

### 6.1 Metrics

- **TPR** — fraction of deliberate bumps producing **exactly one** event.
- **Double-fire rate** — bumps producing 2+ events. Counted **separately** and
  held to a hard zero in the primary tests, because "exactly one event" is the
  requirement, not a quality target.
- **FP rate** — false events per minute (motion tests) or per stimulus (slam
  tests).

Run `z` before every test. Read `s` after. Record the numbers in a table; an
untabulated run did not happen.

### 6.2 The tests

| # | Test | Procedure | SHIP bar | GOOD bar |
|---|---|---|---|---|
| **A** | **In-hand deliberate** | Hold in one hand, knuckle-knock the face with the other. 30 trials, ~1 s apart. | **>= 27/30 (90%)**, **0 double-fires** | >= 29/30 |
| **A2** | **Direction sweep** (L1 anisotropy check) | 10 knocks each on left edge, right edge, back. | **>= 8/10 each** | >= 9/10 each |
| **B** | **Lanyard, standing still** | Badge hanging on a worn lanyard, knock it. 30 trials. | **>= 27/30 (90%)**, 0 double-fires | >= 28/30 |
| **C** | **On a desk** | Badge flat on a desk, knock the badge itself. 20 trials. | **>= 17/20 (85%)** | >= 19/20 |
| **D** | **Lanyard while walking** | Walk a corridor at normal pace **3 minutes**, no deliberate bumps. | **<= 1 FP / 3 min** | **0 FP / 3 min** |
| **E** | **Walking, worst case** | 1 min brisk walk including one flight of stairs. | **<= 2 FP** | <= 1 FP |
| **F** | **Desk while typing** | Badge on the desk beside a laptop, type normally 2 min. | **0 FP** (hard zero) | 0 FP |
| **G** | **Neighbour slams the table** | Badge flat on the same desk; second person palm-slams at ~40 cm, **20 times**, varying hard/soft. | **<= 1 FP / 20** | 0 FP / 20 |
| **G2** | **Slam, lanyard worn seated** | Same, badge on a lanyard worn by someone seated at that desk. 20 slams. | **<= 1 FP / 20** | 0 FP / 20 |
| **H** | **Sit down / stand up** | 10 cycles, badge on lanyard. | **<= 1 FP / 10** | 0 FP / 10 |
| **I** | **Squeeze and sustained push** | Squeeze hard 10x; press and hold against the desk 10x. | **0 FP** (hard zero) | 0 FP |
| **J** | **Venue bass** | Badge on a surface near a loud speaker, 2 min. | **0 FP** | 0 FP |
| **K** | **NFC co-existence** | 20 pause/scan/resume cycles (`x` or real NFC), no deliberate bumps. | **0 FP**, `i2e` growth **<= 2** | 0 FP, `i2e` = 0 |
| **L** | **Starvation under load** | Display doing full-screen updates + LEDs animating. Read `s`. Then re-run A. | `hz` **>= 380**, `late` **<= 5/min**, `gap` **<= 15 ms**, and **A still >= 27/30** | `hz` >= 395, `gap` <= 5 ms |

### 6.3 Reading the bars honestly

- **F, I, J and K are hard zeros.** They are low-amplitude or non-impulsive
  stimuli; a failure there means a gross misconfiguration (threshold far too
  low, or the resume re-seed is not wired), not a tuning subtlety. Fix the bug;
  do not tune around it.
- **D is the bar that decides whether this is shippable.** ≤1 FP in 3 minutes is
  **a demo-grade bar, stated as such**: at 0.33 FP/min a 36-hour event would
  produce ~700 spurious events, which is clearly unacceptable as a product. The
  honest product bar is **≤1 FP/hour**, which is roughly 20x better than the
  ship bar and is not reachable in a 3-hour budget with a polled accelerometer
  and no ground truth. Ship to the demo bar, write down the gap.
- **G is the discriminator-of-record test.** It is the one scenario where
  isolation contributes nothing (§2.1) and amplitude alone may not separate. If
  G fails after the §6.4 sweep, the ringdown test is not buying what §2.4 claims
  and you should say so rather than quietly lowering the bar.
- **E and H are expected to be the hardest.** Stairs and sitting produce
  genuinely knock-like impulses. If they are the only failures, ship anyway and
  document them.
- **L is what converts "it works" into a measured claim.** A detector that
  passes A at 30/30 on an idle badge and 18/30 with the display running has not
  passed anything. **Always re-run A under load.**

### 6.4 The 45-minute tuning loop

```
0-5   BRING-UP GATE. 'z'. Flip the badge through 6 orientations.
      - WHO_AM_I (0x0F) == 0x11
      - 1 g flip test: each axis ~ +/-250 counts at rest  (§1.3)
        reads ~1000 instead? -> FS write failed, go to the §1.8 fallback
      - 's' shows hz >= 380, late = 0, dup ~ 20% of samples
      HARD GATE: do not tune until all three pass. Tuning on a broken
      scale factor or a stale-register read wastes the whole 45 minutes.

5-12  MEASURE, DO NOT GUESS.  'z', then for each, 'c' after / 'C' during:
      - stand still 10 s         -> note mag p99      => BASE
      - walk 30 s                -> note mag p99      => W
      - 10 in-hand knocks        -> note MIN peak     => Kmin
      - 10 neighbour desk slams  -> note MAX peak     => Smax
      Set  thr   = max(1.3 * W, 0.6 * Kmin)
           qgate = 2.5 * BASE
      SANITY: if Kmin < 1.5 * W you have no amplitude margin. Either knock
      harder, or you are clipping -> check for BUMP_F_CLIPPED and re-read
      §1.2. Do not proceed with no margin; no discriminator can rescue it.

12-25 THRESHOLD SWEEP. For thr in {0.6, 0.8, 1.0, 1.2} x your estimate:
        10 in-hand knocks (count hits) + 30 s walk (count FPs)
      Pick the LARGEST thr that still gives 10/10. Write it down. 'S'.

25-35 RINGDOWN TUNE. thr fixed. Run test G (20 slams).
      FPs > 1 ? lower 'h': 10 -> 8 -> 6 -> 4, re-running test A (10 knocks)
      after each. If A drops below 9/10 you went too far -> back off 'h'
      and raise thr instead.
      Still failing at h=4 ? The slam amplitude genuinely overlaps your
      knocks. Accept desk-mode as degraded, DOCUMENT it, move on. Do not
      burn the remaining time here.

35-42 ISOLATION + AGITATION. Run test D (walk 3 min).
      FPs > 1 ? raise 'n': 200 -> 300 -> 400 ms, re-running test B
      (lanyard knocks while standing) after each.
      If that costs true positives, revert 'n' and instead sweep 'a'
      down from 600 (a1 to enable). Measure both; keep whichever wins.

42-45 PERSIST AND VERIFY. 'S' to save. POWER-CYCLE THE BADGE.
      'L' / 's' to confirm every value survived. Re-run 10 in-hand knocks
      as a final smoke test.
      NOT OPTIONAL: unsaved tuning is lost tuning, and a brown-out from
      the LEDs on AA cells will take it from you at the worst moment.
```

**If the loop overruns:** stop at 35 minutes, `S`, and run only tests A, D, F
and L. Those four cover true-positive rate, the primary false-positive threat,
a gross-misconfiguration canary, and the starvation proof. G, H and E can be
documented as untested.

---

## 7. Timebox and cut list

### 7.1 This design: done

Delivered: ODR/FS decision with physical justification, the full fixed-point
signal chain, the discriminator set with a pressure-test of the proposed pair
and one replacement plus one addition, ~110 lines of ESP-IDF C, the header
contract, the console command set, and the acceptance protocol. No further
design iteration should be bought before bench data exists — every remaining
open question in this document is marked **[S]** or **[U]** precisely because
the bench answers it faster than analysis does.

### 7.2 Builder's 3-hour budget

| Window | Work | Gate before moving on |
|---|---|---|
| 0:00–0:30 | `sdkconfig` (FREERTOS_HZ=1000!), add device to shared bus, read loop, `s` stats, 1 g flip test | **`hz` >= 380, flip test passes** |
| 0:30–1:00 | EMA + L1 + threshold-only detector + queue + one dim LED flash on event | **A knock lights an LED** |
| 1:00–1:30 | Isolation, ringdown, refractory, reject lockout | Squeeze and typing produce nothing |
| 1:30–1:50 | Console set/get, `s`, `z`, **NVS `S`/`L`** | A value survives a power cycle |
| 1:50–2:35 | The §6.4 tuning loop | `S` saved |
| 2:35–3:00 | Acceptance tests A, D, F, G, L. Record numbers. | Numbers written down |

### 7.3 Cut list, strictly bottom-up

1. **Cut first: continuous CSV (`C`) and the ring dump (`c`).** Keep `s`. You
   can tune from peak and stat readouts alone — slower, but it works. Saves
   ~15 min.
2. **Cut second: the agitation veto.** Ship `agit_en=0` and set
   `BUMP_F_UNVETTED`. Amplitude + isolation + ringdown is enough for a demo, and
   this also removes a whole tuning axis from the 45-minute loop. Saves ~10 min
   of code and ~7 of tuning.
3. **Cut third: the `axis` hint and `flags`.** Keep `t_us` + `peak` + `seq`.
   Saves ~5 min. (Keep the struct fields, just stop populating them, so the
   consumer's code does not change.)
4. **Cut fourth, and only under real pressure: the ringdown test.** Ship
   amplitude + isolation + refractory. This is last because losing it reopens
   squeeze/push and table-slam false positives — but a squeeze is a *deliberate*
   action a user will not perform by accident, so the practical loss is mostly
   test G. Saves ~20 min.

### 7.4 Never cut these four

- **The refractory.** It *is* the "exactly one event" requirement. Without it a
  single knock delivers 3–5 events and the feature is worse than absent.
- **NVS config save.** Without it, tuning does not survive a power cycle or a
  LED brown-out, and you tune twice. Cheapest possible insurance.
- **The health counter (`hz`/`late`/`gap`/`dup`).** It is the only thing that
  tells you *why* the detector broke on a crowded single core. Debugging a
  starved detector without it costs more than it costs to build.
- **Bounded I2C timeouts.** The badge spec explicitly warns the MFRC522 can
  wedge this bus on droopy battery power. An unbounded wait turns a flaky bus
  into a hung badge.

### 7.5 Double-tap and gesture vocabularies: agreed, stay cut

Not a reluctant concession — the trade is actively bad:

1. **It degrades the single tap, which is the feature that matters.** Seeing a
   second tap requires the refractory to drop below ~150 ms, which directly
   reopens the double-fire failure mode on single taps (§2.6 constraint 2).
2. **It adds latency to every tap.** You must wait out the full double-tap
   window (~300 ms) before emitting a single-tap event, because you cannot know
   yet whether a second is coming. Single-tap latency goes from ~65 ms to
   ~360 ms — from "instant" to "laggy" — for every user, all the time.
3. **Discovery is near zero.** Nobody finds an undocumented double-tap gesture
   at a 36-hour event.
4. **It doubles the tuning surface** in a budget that is already tight for one
   gesture.

**The escape hatch costs nothing and should be mentioned to the consumer:**
`t_us` and `seq` in the event struct let application code implement double-tap
later with zero firmware change — at a 250 ms refractory, two taps 300–600 ms
apart arrive as two clean events and the UI can group them if it ever wants to.

---

## 8. Open items for the builder to close on the bench

| Item | Marked | How to close | Time |
|---|---|---|---|
| SC7A20 FS bits behave like LIS3DH | **[U]** | §1.3 flip test | 30 s |
| Internal anti-alias filter is real | **[U]** | §1.7 peak-spread test | 2 min |
| 32-level FIFO exists | **[U]** | §1.9 runtime probe | 10 min |
| `thr`, `qgate` absolute values | **[S]** | §6.4 steps 5-12 and 12-25 | 20 min |
| `dhot_max` | **[S]** | §6.4 step 25-35 (test G) | 10 min |
| `qneed` vs `agit_max` trade | **[S]** | §6.4 step 35-42 (test D vs B) | 7 min |
| Does ringdown actually beat a slam | **design claim** | Test G. **If it fails at `h`=4, say so** rather than lowering the bar. | — |
