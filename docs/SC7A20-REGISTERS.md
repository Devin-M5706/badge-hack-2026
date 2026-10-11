# SC7A20HTR register facts for the HTN 2026 badge (ESP32-C3)

**Verdict up front: the real SC7A20 datasheet EXISTS and I read it.**
Hangzhou Silan Microelectronics, `SC7A20_Datasheet`, **Rev. 1.3, 27 pages**.
Retrieved from `https://www.compel.ru/item-pdf/5969b6fe68464f2f6df6196ab215ac18/pn/silan~sc7a20tr.pdf`
(local copies: `scratchpad/sc7a20.pdf`, text `scratchpad/sc7a20.txt`).

It is **not** a reskinned LIS3DH datasheet. It has its own parametric tables, its own
block diagram, and at least two values that **differ from ST**. It is also
**incomplete**: the register-description chapter stops at `0x37`, so the click
engine and FIFO registers are named in the address map but never defined.

## Confidence legend

| Label | Meaning |
|---|---|
| **[BADGE GUIDE]** | `C:\Users\dmyer\.claude\reference\htn-badge-esp32-c3.md` (board truth) |
| **[SC7A20 DATASHEET]** | Silan SC7A20 Rev 1.3 - proven for this part |
| **[LIS3DH FAMILY, INFERRED]** | ST LIS3DH DS Doc ID 17530 Rev 1 / AN3308 Doc ID 18198 Rev 1 - assumed to carry over, **not** proven for SC7A20 |
| **[UNKNOWN - MUST PROBE]** | No documentation for this part; bench it |

Reference sources: ST **LIS3DH** datasheet Doc ID 17530 Rev 1 (`scratchpad/lis3dh.pdf`),
ST **AN3308** Doc ID 18198 Rev 1 (`scratchpad/an3308.pdf`), NXP **AN4072** (tap
detection, for Q3), Linux `st_accel` SC7A20 patch (`scratchpad/sc7a20_iio.txt`),
ESP-IDF driver `mydazy/esp_sc7a20h` (`scratchpad/sc7a20h.c`).

---

## HEADLINE CORRECTIONS - read these before touching the bench

1. **The badge guide's `CTRL_REG4 = 0x80` is NOT 12-bit mode.** `0x80` sets BDU
   only; **HR (bit 3) = 0**, which is 10-bit normal mode. The guide's own
   "12-bit left-justified, 1 count = 1 mg" line contradicts the value it tells
   you to write. Use **`0x88`** for true 12-bit at +-2 g. The `raw >> 4`
   arithmetic still yields 1 mg per count either way, but with HR=0 the value
   only ever moves in steps of 4 -> **4 mg effective resolution**.
2. **+-16 g is NOT 12 mg/count on this part.** The SC7A20 datasheet says
   **8 mg/digit**. ST's LIS3DH says 12. Upstream Linux `st_accel` copied ST's 12.
   Do not use +-16 g without bench-calibrating it (details in Q2).
3. **Max specified ODR is 400 Hz**, not 1.25/1.6/5 kHz. The register codes for
   those exist but are not in any parametric table (Q1).
4. **The click engine's register bit layouts are undocumented for SC7A20.** The
   feature is vendor-claimed in four separate places, but section 12 of the
   datasheet ends at `0x37`. No public driver has ever exercised it (Q5).
5. **Neither INT pin is routed on the badge** -> everything embedded must be
   polled (Q7).

---

## Q1 - CTRL_REG1 (0x20) ODR codes

**Bit layout - [SC7A20 DATASHEET] 12.1, verbatim:**

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| ODR3 | ODR2 | ODR1 | ODR0 | LPen | Zen | Yen | Xen |

**ODR code table - [SC7A20 DATASHEET] 12.1, verbatim:**

| ODR3:0 | Mode / rate | `CTRL_REG1` with all axes on, LPen=0 |
|---|---|---|
| `0000` | Power-down | `0x07` |
| `0001` | Normal / low-power (1 Hz) | `0x17` |
| `0010` | Normal / low-power (10 Hz) | `0x27` |
| `0011` | Normal / low-power (25 Hz) | `0x37` |
| `0100` | Normal / low-power (50 Hz) | `0x47` |
| `0101` | Normal / low-power (**100 Hz**) | **`0x57`** |
| `0110` | Normal / low-power (**200 Hz**) | **`0x67`** |
| `0111` | Normal / low-power (**400 Hz**) | **`0x77`** |
| `1000` | low-power only (1.6 kHz) | `0x87` |
| `1001` | Normal (1.25 kHz) / low-power (5 kHz) | `0x97` |

**Your beliefs - both CONFIRMED. [SC7A20 DATASHEET]**
- `0x57` = 100 Hz, all axes, normal mode. **Correct.**
- `0x77` = 400 Hz, all axes, normal mode. **Correct.**
- **200 Hz = `0x67`.** **400 Hz = `0x77`.**

**Max ODR - [SC7A20 DATASHEET], and this is a divergence from LIS3DH:**

The electrical-characteristics table characterises **only** `ODR0..ODR6`
= **1, 10, 25, 50, 100, 200, 400 Hz**. There is **no** parametric entry for the
`1000` or `1001` codes. The datasheet's own feature text says the part is
"capable of measuring accelerations with output data rates **from 1 Hz to
400 Hz**".

-> **Treat 400 Hz as the maximum in BOTH normal and low-power mode.** The
`1000`/`1001` codes are present in the register table but unspecified - and
note the SC7A20 numbers (1.25 kHz / 5 kHz) are *different* from LIS3DH's
(1.344 kHz / 5.376 kHz), so they are not a copy-paste and may be a different
clock divider entirely. **[UNKNOWN - MUST PROBE]** if you ever need >400 Hz.

Corroboration: the Linux `st_accel` SC7A20 entry lists ODRs up to 1600 Hz
(`value = 0x08`) and **omits `0x09` entirely** - i.e. upstream also did not
trust the top code.

For reference, LIS3DH is identical for `0000`-`1000` and differs only at
`1001`: "Normal (1.344 kHz) / low-power mode (5.376 kHz)". **[LIS3DH FAMILY]**

---

## Q2 - CTRL_REG4 (0x23) full scale, and the mg/count table

**Bit layout - [SC7A20 DATASHEET] 12.4, verbatim:**

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| BDU | BLE | FS1 | FS0 | HR | ST1 | ST0 | SIM |

- **FS1:FS0 are bits 5:4 - CONFIRMED. [SC7A20 DATASHEET]**
- **`00: +/-2 G; 01: +/-4 G; 10: +/-8 G; 11: +/-16 G` - CONFIRMED verbatim. [SC7A20 DATASHEET]**
- `BDU` - "0: continuous update, 1: output is not updated until MSB and LSB are read"
- `BLE` - "0: low-byte data is in low-byte address" -> **BLE=0 is little-endian**, matches the badge guide
- `HR` - "High resolution output mode selection. Default value: 0 (0: high resolution disable; 1: high resolution enable)"

### Your `0xA0` for BDU + +-8 g

**Arithmetically right, functionally wrong.** `0xA0` = BDU=1, BLE=0, FS=`10`
(+-8 g), **HR=0**. That is 10-bit normal mode.

| Intent | Value | Notes |
|---|---|---|
| BDU + +-8 g, **true 12-bit** | **`0xA8`** | HR=1 - use this |
| BDU + +-8 g, 10-bit normal | `0xA0` | your value |
| BDU + +-2 g, **true 12-bit** | **`0x88`** | the badge guide's `0x80` with HR added |
| BDU + +-2 g, 10-bit normal | `0x80` | **what the badge guide actually tells you to write** |

### mg per count, 12-bit left-justified HR mode

**[SC7A20 DATASHEET]** - MECHANICAL CHARACTERISTICS (VDD=2.5 V, TA=25 degC),
verbatim. Note every row is explicitly qualified **"(HR mode)"**:

| Symbol | Test condition | FS | Typ. | Unit |
|---|---|---|---|---|
| So0 | FS=0 (HR mode) | +-2 g | **1** | mg/digit |
| So1 | FS=1 (HR mode) | +-4 g | **2** | mg/digit |
| So2 | FS=2 (HR mode) | +-8 g | **4** | mg/digit |
| So3 | FS=3 (HR mode) | +-16 g | **8** | mg/digit |

**Your expectation was 1 / 2 / 4 / 12. The first three are CONFIRMED. The
fourth is REFUTED for this part - the SC7A20 datasheet says 8, not 12.**

This is a genuine part difference, not a typo, and both numbers are internally
self-consistent:

| Part | +-16 g sensitivity | x 2048 counts | Implied true range |
|---|---|---|---|
| SC7A20 | 8 mg/digit **[SC7A20 DATASHEET]** | 16,384 mg | **+-16.4 g** - a real +-16 g |
| LIS3DH | 12 mg/digit **[LIS3DH FAMILY]** (DS Doc ID 17530 Rev 1, "FS bit set to 11 -> 12 mg/digit") | 24,576 mg | +-24.6 g - ST's "+-16 g" is really +-24 g |

**But it is CONTESTED in the wild:** the Linux kernel `st_accel` SC7A20 entry
uses `.gain = IIO_G_TO_M_S_2(12000)` for `ST_ACCEL_FS_AVL_16G` - i.e. upstream
took ST's 12 mg/digit for the Silan part. The patch's own comment is
*"Not an ST part. Register-compatible with the LIS2DH, even though the WAI value
is different"* - it was added by register analogy, and nothing in the thread
says the +-16 g scale was measured.

-> **Practical guidance:** stay at +-2/4/8 g where the datasheet and ST agree
exactly (1/2/4 mg). If you need +-16 g, **bench-calibrate it**: lay the badge
flat, read Z, flip it, read Z, and check the +-1 g delta against both 8 and
12 mg/count. 50 % error in a g-force readout is exactly the kind of thing that
eats an afternoon.

### Must HR be set for true 12-bit? - YES

**[SC7A20 DATASHEET]** Two independent reasons:
1. `CTRL_REG4` bit 3 is literally named "High resolution output mode selection",
   default 0 = disabled.
2. The sensitivity table gives sensitivity **only** for HR mode. The datasheet
   publishes **no** non-HR sensitivity figure at all - so HR=1 is the only
   operating point whose scale factor is specified for this part.

The front-page feature list also says "**12-bit effective data (HR)**" - the
parenthetical is the vendor's own warning that 12 bits is an HR-mode property.

### What LPen (CTRL_REG1 bit 3) does to resolution

**[SC7A20 DATASHEET]** says only: *"Low power dissipation enable, default value: 0.
(0: normal mode; 1: low power dissipation mode)"* - **it never states a bit depth
for any mode.**

**[LIS3DH FAMILY, INFERRED]** - the three-mode ladder:

| LPen | HR | Mode | Valid bits | Data position |
|---|---|---|---|---|
| 1 | x | Low-power | 8 | left-justified, `OUT_x_H` only |
| 0 | 0 | Normal | 10 | left-justified in the 16-bit word |
| 0 | 1 | **High resolution** | **12** | left-justified in the 16-bit word |

And the LIS3DH per-mode sensitivity ladder, **[LIS3DH FAMILY, INFERRED]** for
SC7A20 (the LIS3DH DS itself prints only the HR column; the 10-/8-bit rows come
from the family app notes and the LIS2DH12 DS):

| FS | HR 12-bit | Normal 10-bit | Low-power 8-bit |
|---|---|---|---|
| +-2 g | 1 mg | 4 mg | 16 mg |
| +-4 g | 2 mg | 8 mg | 32 mg |
| +-8 g | 4 mg | 16 mg | 64 mg |
| +-16 g | 12 mg (**SC7A20 says 8**) | 48 mg | 192 mg |

Because the data stays left-justified in all three modes, **`counts = raw >> 4`
with 1 mg/count at +-2 g is numerically correct in every mode** - the modes
differ only in how many low bits are real. In normal mode the value steps by 4;
in low-power mode by 16. The badge guide's formula is therefore not *wrong*,
it is just quietly 4x coarser than it claims. **[LIS3DH FAMILY, INFERRED]** -
that SC7A20 left-justifies identically in non-HR modes is **not** documented.
One more reason to just set HR=1.

---

## Q3 - Does +-2 g actually clip on a bump?

**You are mostly right, but "every usable bump saturates" is an overstatement.
The honest answer depends on whether it is a deliberate knock or a light tap.**

### Hard numbers from NXP AN4072 (the industry tap-detection app note)

Every one of these is a **worked threshold example from the vendor**, so the real
peak must be *above* the number quoted:

| AN4072 example | X / Y threshold | Z threshold |
|---|---|---|
| 4.2.1 generic | 2 g | 3 g |
| 6.1 single tap, 400 Hz | 1.575 g | 2.65 g |
| 6.2 double tap, 400 Hz | 3 g | 5 g |
| 7.2 directional tap | 1.67 g | 3.19 g |

NXP's own setup instruction for tap work: *"Configure the accelerometer to be in
**800 Hz 8g Mode**"*, and *"In order to detect the full signature of a tap a
sample rate of **600 Hz or greater** is typically required."* Their threshold
register is deliberately scaled **0.063 g/LSB at a fixed 8 g range** - i.e. the
threshold field alone is built to express up to 8 g.

### The other end of the range

- ST/Adafruit's LIS3DH guidance recommends click thresholds of **40-80 LSB at
  +-2 g**, and `1 LSB = FS/128 = 15.625 mg` -> **0.63-1.25 g**. So ST's own
  default tuning for a *light* tap sits comfortably inside +-2 g.
- A handheld-device tap-detection patent (CA2691899C / US8482520) reports a
  measured tap peaking at about **0.5 g**, rising to peak in **~5 ms**.

### Verdict

| Stimulus | Typical peak | Clips at +-2 g? |
|---|---|---|
| Light fingertip tap on the screen/case | ~0.5-1.2 g | **No** |
| Deliberate finger tap, side of board | ~1.5-2.7 g | **Marginal - Z likely clips** |
| Knuckle knock | ~3-5 g+ | **Yes, hard** |

**And the badge-specific killer:** at +-2 g with the badge lying flat, Z already
carries **+1 g of gravity**, so you only have **1 g of dynamic headroom on Z**
before the ADC rails - and Z is exactly the axis a knock normal to the PCB
excites hardest (note how every NXP example sets the Z threshold ~2x the X/Y
one). You also cannot even *express* a useful threshold: `CLICK_THS` is 7 bits
of `FS/128`, so at +-2 g its maximum is `127 x 15.625 mg = 1.98 g`.

-> **Recommendation: run the click/tap work at +-8 g (`CTRL_REG4 = 0xA8`), ODR
400 Hz (`CTRL_REG1 = 0x77`).** 4 mg/count is still far finer than any tap
decision needs, you get 7 g of headroom on Z, and `CLICK_THS` can reach 7.94 g.
Reserve +-2 g for tilt/orientation, where the extra resolution actually pays.

---

## Q4 - CTRL_REG2 (0x21) high-pass filter

**Bit layout - [SC7A20 DATASHEET] 12.2, verbatim. Identical to LIS3DH:**

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| HPM1 | HPM0 | HPCF2 | HPCF1 | FDS | **HPCLICK** | HPIS2 | HPIS1 |

**[SC7A20 DATASHEET]** field descriptions, verbatim:
- `HPM1-HPM0` - "High pass mode. Default value: 00"
- `HPCF2-HPCF1` - "High pass cutoff frequency selection" *(no table given - see below)*
- `FDS` - "Data filtering selection. Default value: 0 (0: skip internal filtering;
  1: data is output to data register or FIFO after filtering"
- **`HPCLICK` (bit 2)** - "**CLICK High pass filtering enable** (0: filtering
  disable; 1: filtering enable)" -> **CONFIRMED: bit 2, routes the filtered
  signal to the click engine. [SC7A20 DATASHEET]**
- `HPIS2` (bit 1) - "Interrupt 2 AOI high pass filtering enable"
- `HPIS1` (bit 0) - "Interrupt 1 AOI high pass filtering enable"

**HPM mode table - [SC7A20 DATASHEET] 12.2, verbatim:**

| HPM1 | HPM0 | High-pass filtering mode |
|---|---|---|
| 0 | 0 | Normal mode (reading high-pass filtering is reset automatically) - reset by reading `REFERENCE (0x26)` |
| 0 | 1 | Filtering reference signal |
| 1 | 0 | Normal mode |
| 1 | 1 | Interrupt event automatic reset |

Matches LIS3DH AN3308 Table 7 word for word.

### HPCF cutoff frequencies - NOT in the SC7A20 datasheet

**The SC7A20 datasheet names the field and gives no table anywhere in all 27
pages.** I grepped the full text: the only hit for "cutoff" is the one-line
field description. So:

**[LIS3DH FAMILY, INFERRED]** - AN3308 Doc ID 18198 Rev 1, Table 8. The rule is
`ft = ODR / {50, 100, 200, 400}`:

| HPCF2:HPCF1 | Divider | ft @ 100 Hz | **ft @ 200 Hz** | **ft @ 400 Hz** |
|---|---|---|---|---|
| `00` | ODR/50 | 2 Hz | **4 Hz** | **8 Hz** |
| `01` | ODR/100 | 1 Hz | **2 Hz** | **4 Hz** |
| `10` | ODR/200 | 0.5 Hz | **1 Hz** | **2 Hz** |
| `11` | ODR/400 | 0.2 Hz (table prints 0.2, exact 0.25) | **0.5 Hz** | **1 Hz** |

AN3308 titles this "Low power mode - high-pass filter cut-off frequency"; the
divider ratio is the same in normal/HR mode on LIS3DH.

**For tap work use `HPCF = 00`** (8 Hz at 400 Hz ODR): high enough to kill
gravity and slow tilt, far below the tap's energy band.

**Recommended value: `CTRL_REG2 = 0x04`** = HPM `00`, HPCF `00`, FDS=0 (output
registers stay unfiltered so your g readout still sees gravity), **HPCLICK=1**.
This is the "filtered data to the click engine, raw data to the output
registers" split that AN3308 Figure 2 describes.

---

## Q5 - The click/tap engine

### What is PROVEN about the SC7A20 click registers

**[SC7A20 DATASHEET]** register address map (11) lists them - and **that is all
it says about them**:

| Name | Type | Addr | Default | Bit layout in datasheet? |
|---|---|---|---|---|
| `CLICK_CFG` | rw | `0x38` | `00000000` | **NO** |
| `CLICK_SRC` | r | `0x39` | `00000000` | **NO** |
| `CLICK_THS` | rw | `0x3A` | `00000000` | **NO** |
| `TIME_LIMIT` | rw | `0x3B` | `00000000` | **NO** |
| `TIME_LATENCY` | rw | `0x3C` | `00000000` | **NO** |
| `TIME_WINDOW` | rw | `0x3D` | `00000000` | **NO** |
| `ACT_THS` | rw | `0x3E` | `--` | **NO** |
| `ACT_DURATION` | rw | `0x3F` | `--` | **NO** |

**Chapter 12 ("Register description") runs 12.1 -> 12.18 and stops at
`INT2_DURATION (0x37)`. The next page is "TYPICAL APPLICATION CIRCUIT".** There
is no 12.19. The click engine, the FIFO registers and the activity registers are
all named and never defined. The datasheet defers three times to an
*"application note"* that I could not find published anywhere.

### Bit layouts - [LIS3DH FAMILY, INFERRED]

From ST LIS3DH DS Doc ID 17530 Rev 1, Tables 55-66 (read directly from the PDF):

**`CLICK_CFG (0x38)`**

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| -- | -- | ZD | ZS | YD | YS | XD | XS |

`xD` = enable **double**-click on that axis, `xS` = enable **single**-click.
Truth table (AN3308 Table 20): `D=0,S=1` -> single; `D=1,S=0` -> double;
`D=1,S=1` -> single or double.
-> **single click, all three axes = `0x15`**; all six enables = `0x3F`.

**`CLICK_SRC (0x39)`** - read-only

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| -- | **IA** | DCLICK | SCLICK | Sign | Z | Y | X |

- **`IA` (bit 6) = "Interrupt active" - this is the bit that signals an
  interrupt-active click.** "0: no interrupt has been generated; 1: one or more
  interrupts have been generated".
- **An interrupt-active SINGLE click = `IA` (bit 6) AND `SCLICK` (bit 4) both
  set**, i.e. `src & 0x50 == 0x50`. Mask `0x40` alone tells you *something*
  fired; bit 4 vs bit 5 tells you single vs double.
- `Sign` (bit 3): 0 = positive, 1 = negative.
- `Z`/`Y`/`X` (bits 2/1/0): which axis.
- **Reading `CLICK_SRC` clears it.**

**`CLICK_THS (0x3A)`**

| B7 | B6..B0 |
|---|---|
| `LIR_Click` *(see caveat)* | `Ths6..Ths0` |

- **`1 LSB = full scale / 128`** - AN3308 8.3.3, verbatim: *"1 LSB = full
  scale/128."* **[LIS3DH FAMILY, INFERRED]**
  - **Strong corroboration inside the SC7A20 datasheet itself:** 12.17
    `INT2_THS` states *"1LSB=16mg @ FS=2g / 1LSB=32mg @ FS=4g / 1LSB=64mg @
    FS=8g / 1LSB=128mg @ FS=16g"* - that **is** `FS/128` (15.625/31.25/62.5,
    rounded). So the FS/128 threshold scaling is proven for this part's AOI
    thresholds, which makes it very likely for `CLICK_THS` too. Still an
    inference, but a well-supported one.
  - Values: `+-2 g` -> 15.625 mg/LSB (max 1.98 g). `+-8 g` -> 62.5 mg/LSB
    (max 7.94 g). **1.5 g at +-8 g = 24 = `0x18`.**
- **`LIR_Click` caveat:** bit 7 is shown as reserved `-` in LIS3DH DS Rev 1, and
  is defined as `LIR_Click` (latch click interrupt request) in **LIS2DH12** and
  later LIS3DH revisions. For SC7A20 it is **[UNKNOWN - MUST PROBE]**.
  **This matters for the badge** - see the latching note below.

**`TIME_LIMIT (0x3B)`** - bit 7 reserved, `TLI6..TLI0`. **`1 LSB = 1/ODR`.**
Max time between the signal crossing the threshold and coming back under it.

**`TIME_LATENCY (0x3C)`** - full 8 bits `TLA7..TLA0`. **`1 LSB = 1/ODR`.**
Dead time after a detected click during which detection is disabled.

**`TIME_WINDOW (0x3D)`** - full 8 bits `TW7..TW0`. **`1 LSB = 1/ODR`.**
Window after the latency in which a second click still counts as a double.

**The `1/ODR` unit is CONFIRMED arithmetically** from AN3308's own figure
captions, all captured at ODR = 400 Hz (so 1 LSB should be 2.5 ms):

| AN3308 caption | Register value | Stated time | 2.5 ms x value |
|---|---|---|---|
| Fig 17 | `TimeLimit = 0x01` | 2.5 ms | 2.5 ms OK |
| Fig 18 | `TimeLimit = 0x33` (51) | 127 ms | 127.5 ms OK |
| Fig 19 | `Latency = 0x15` (21) | 52 ms | 52.5 ms OK |
| Fig 20 | `Latency = 0xFF` (255) | 637 ms | 637.5 ms OK |
| Fig 22 | `Window = 0xFF` (255) | 637 ms | 637.5 ms OK |

(AN3308 Fig 21 says `Window = 0x42` is "1065 ms"; 66 x 2.5 ms = 165 ms. That
caption is a typo in ST's document - the other five all check out.)

**Routing bits - these ARE documented for SC7A20:**
- `CTRL_REG3 (0x22)` bit 7 = **`I1_CLICK`** - "CLICK interrupt on INT1. Default
  value: 0" **[SC7A20 DATASHEET]**
- `CTRL_REG6 (0x25)` bit 7 = **`I2_CLICK`** - "CLICK interrupt on INT2. Default
  value: 0" **[SC7A20 DATASHEET]**
- `CTRL_REG6` bit 1 = `H_LACTIVE` - "0: interrupt triggered by high level;
  1: interrupt triggered by low level" **[SC7A20 DATASHEET]**

**Latching / polling behaviour - [LIS3DH FAMILY, INFERRED], and critical here:**
- `LIR_Click = 0` -> the click interrupt is held **for the duration of the
  latency window** (`TIME_LATENCY`), then self-clears.
- `LIR_Click = 1` -> held **until `CLICK_SRC` is read**.

Since the badge routes no INT pin (Q7), a polled loop can **miss** a click whose
assertion is shorter than the poll interval. Two mitigations, use both:
1. Try `LIR_Click = 1` (set bit 7 of `CLICK_THS`, e.g. `0x98` for a 24-count
   threshold) - but verify it, since it is undocumented for SC7A20.
2. Set `TIME_LATENCY` long enough to cover your poll period regardless - e.g.
   `0x28` (40 x 2.5 ms = **100 ms**) at 400 Hz. This also debounces.

### Is there evidence the SC7A20 clone implements the engine at all?

**Vendor-claimed in four independent places. Never verified by anyone public.**

**Evidence FOR - [SC7A20 DATASHEET]:**
1. DESCRIPTION, p.1: *"interrupt source include 6D/4D direction detection
   interrupt signal, free fall detection interrupt signal, sleep and wake up
   detection interrupt signal, and **single/double click detection interrupt
   signal**"*
2. FEATURES, p.1: *"**Single/double click detection** and motion detection"*
3. 8, a dedicated functional section titled **"Single click and double click
   detection"**: *"The sensor judges whether the output reaches conditions for
   single/double click according to given threshold value and time, sets
   corresponding state signal and generates interrupt signal. Refer to
   application note for details."*
4. Three *supporting* config bits are fully documented: `HPCLICK` (CTRL_REG2
   bit 2), `I1_CLICK` (CTRL_REG3 bit 7), `I2_CLICK` (CTRL_REG6 bit 7). A clone
   that stubbed out the engine would have no reason to document three bits whose
   only purpose is to feed and route it.
5. All six registers appear in the address map with correct `rw`/`r` types and
   `0x00` defaults.

**Evidence AGAINST / unresolved:**
1. **No bit-level definition for `0x38`-`0x3D` anywhere in Rev 1.3.** Chapter 12
   stops at `0x37`. The datasheet punts to a non-public application note.
2. **No public driver has ever touched the click path.** I checked the three
   real SC7A20 codebases:
   - `mydazy/esp_sc7a20h` (ESP-IDF component, README says "Register-compatible
     with ST LIS2DH12 / LIS3DH") defines **only** `WHO_AM_I 0x0F`,
     `CTRL_REG1..6 0x20-0x25`, `OUT_X_L 0x28`, `INT1_CFG 0x30`,
     `INT1_THS 0x32`, `INT1_DURATION 0x33`. **No `CLICK_*` and no `FIFO_*`
     symbols exist in the file.** Its one example is `basic_motion_wakeup`,
     which uses the AOI1 path, not the click engine.
   - Linux `st_accel` SC7A20 entry declares only `odr`, `pw`, `enable_axis`,
     `fs`, `bdu`, `drdy_irq`, `sim`. No click, no FIFO.
   - IronOS (TS80P/Pinecil) and the ATCwatch P8B port use the data path only.
     The ATCwatch thread confirms "the Sparkfun Arduino LIS3DH driver worked",
     but only for raw XYZ and a hand-raise gesture computed in software.
3. A public WIP commit exists titled *"WIP of click code, but I have SC7A20H,
   not SC7A20, and that's the problem..."*
   (`git.trueserve.org/trueControl/dc34-retro-doom-oled`, commit `e481dd2`).
   **That server returned 503 and I could not read the diff.** The title is a
   real signal that someone hit an SC7A20H-vs-SC7A20 problem while writing
   click code - but I will not guess what it says. **Worth retrying.**

**Conclusion: the existence of the engine is [SC7A20 DATASHEET]-claimed; the
register contract to drive it is [UNKNOWN - MUST PROBE].** Budget for it
working, but gate it behind the probe below and keep a software tap detector as
the fallback.

---

## Q6 - Is there a FIFO?

**Stronger evidence than the click engine - the FIFO block is drawn on the die
diagram.**

**[SC7A20 DATASHEET] - PROVEN:**
- FEATURES, p.1: *"**Embedded FIFO**"*
- **Block diagram, p.2, labels a block "`32 Level FIFO`"** -> **depth 32 is
  proven for this part**, not inferred.
- `CTRL_REG5 (0x24)` is fully defined in 12.5:

| B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0 |
|---|---|---|---|---|---|---|---|
| BOOT | **FIFO_EN** | -- | -- | LIR_INT1 | D4D_INT1 | LIR_INT2 | D4D_INT2 |

  -> **`FIFO_EN` is bit 6 - CONFIRMED.** Verbatim: *"FIFO enable. Default value: 0
  (0: FIFO disable; 1: FIFO enable)"*. So `CTRL_REG5 = 0x40` enables the FIFO.
- `CTRL_REG3 (0x22)` bits 2 and 1 are `HI1_WTM` and `I1_OVERRUN`, both described
  as "FIFO interrupt on INT1" - i.e. watermark and overrun interrupts exist.
- `CTRL_REG2` `FDS` description mentions *"data is output to data register **or
  FIFO** after filtering"*.
- `FIFO_CTRL_REG` `rw` `0x2E` default `00000000` and `FIFO_SRC_REG` `r` `0x2F`
  are both in the address map.
- **An SC7A20-specific FIFO quirk that is NOT in any ST document** (10.2,
  verbatim): *"When SPI mode is used to read three-axis FIFO data continuously,
  it needs to read from the register of **0x27**, read **7 bytes** of data
  continuously, and take the **last 6 bytes** to splice into three-axis data."*
  Someone wrote that because they had silicon in hand with a real FIFO. It is
  SPI-only; the I2C equivalent is undocumented (presumably a burst from `0x28`
  with the auto-increment bit).

**NOT in the SC7A20 datasheet:** the bit fields of `0x2E` and `0x2F`. Chapter 12
never defines them.

**[LIS3DH FAMILY, INFERRED]** - ST LIS3DH DS Tables 42-45:

**`FIFO_CTRL_REG (0x2E)`**

| B7 | B6 | B5 | B4..B0 |
|---|---|---|---|
| FM1 | FM0 | TR | FTH4..FTH0 |

| FM1 | FM0 | Mode |
|---|---|---|
| 0 | 0 | **Bypass** (FIFO off/flush) |
| 0 | 1 | **FIFO** (fills to 32, then stops) |
| 1 | 0 | **Stream** (fills to 32, then overwrites oldest) |
| 1 | 1 | **Stream-to-FIFO / Trigger** |

`TR` - trigger select: 0 = trigger on INT1, 1 = trigger on INT2.
`FTH[4:0]` - watermark level.

**`FIFO_SRC_REG (0x2F)`** - read-only

| B7 | B6 | B5 | B4..B0 |
|---|---|---|---|
| WTM | OVRN_FIFO | EMPTY | FSS4..FSS0 |

`FSS[4:0]` = number of unread samples. **Depth = 32 slots of X,Y,Z**
(LIS3DH DS: *"32 slots of 10data for X, Y and Z"*; SC7A20 block diagram:
*"32 Level FIFO"*).

**Clone question:** the FIFO is **better attested than the click engine** -
block-diagram label, a documented enable bit, two documented FIFO interrupt
bits, and an SC7A20-original FIFO read note. Only the mode/watermark encoding is
inferred, and that is cheap to probe: set `FIFO_CTRL_REG = 0x80` (Stream),
`CTRL_REG5 = 0x40`, wait 100 ms at 100 Hz, then read `FIFO_SRC_REG`. If
`FSS[4:0]` reads a non-zero, growing-then-pinned-at-31 value and `EMPTY` clears,
the FIFO is real and the mode bits decode as ST's. Note the stream-to-FIFO
trigger depends on INT1/INT2, which the badge does not route - so **Stream mode
is the only FIFO mode fully usable here**.

---

## Q7 - Interrupt pins: confirmed NOT routed

**[BADGE GUIDE] 2 pin map, complete and verbatim:**

| Function | GPIO |
|---|---|
| LCD MOSI | 10 |
| LCD CLK | 1 |
| LCD CS | 2 |
| LCD DC | 0 |
| LCD RST | 4 |
| **I2C SDA** | **5** |
| **I2C SCL** | **6** |
| HC165 DATA | 7 |
| HC165 LOAD | 20 |
| HC165 CLK | 21 |
| START button | 9 |
| LED DIN (WS2812) | 3 |

**CONFIRMED: there is no accelerometer interrupt line in the badge pin map.**
The accelerometer's only entries are SDA=5 and SCL=6. Three further
confirmations from the guide itself:
- 5 prescribes **polling**: *"Poll `STATUS (0x27)` for ZYXDA (bit 3)"* - not an
  interrupt-driven flow.
- 0 lists the accelerometer as "SC7A20HTR on shared I2C, address `0x19`" with
  no interrupt.
- 10's HAL checklist has no interrupt step for the accelerometer.

The chip does have the pins - **[SC7A20 DATASHEET]** typical application circuit
shows `INT1` on pin 5 and `INT2` on pin 6 of the LGA-12 - they are simply not
brought to the MCU.

-> **Any embedded function (click, FIFO watermark, AOI, 6D, free-fall) must be
POLLED over I2C.** `CLICK_SRC (0x39)`, `FIFO_SRC_REG (0x2F)` and
`INT1_SOURCE (0x31)` are all readable over the bus, so this is a performance
question, not a capability question.

**One honest caveat:** the guide says *"Hardware design files are not published
yet"*, so this is "no INT appears in the published pin map and the guide's own
method is polling", not "I read the schematic". **GPIO8 is the one ESP32-C3 pin
absent from the map** (GPIO18/19 are the native USB pair). If you want to be
thorough, configure GPIO8 as an input with no pull and watch it while tapping -
if it toggles in sync with taps, an INT line is routed after all. Low
probability, 60 seconds to rule out.

### Polling cost at 400 kHz

A 1-byte register read is `START | addr+W | ACK | sub-addr | ACK | rSTART |
addr+R | ACK | data | NACK | STOP` = **36 clocked bits** plus ~3 bit-times of
START/repeated-START/STOP framing.

At 400 kHz, 1 bit = **2.5 us**:

| Transaction | Clocked bits | Wire time |
|---|---|---|
| **1-byte register read** | 36 | **90 us** (~98 us with framing) |
| 1-byte register write | 27 | 68 us (~75 us) |
| 6-byte burst from `0x28` (auto-increment) | 81 | 203 us (~210 us) |
| 1-byte `CLICK_SRC` + 6-byte XYZ burst | 117 | **293 us** (~310 us) |

Bus occupancy for a bare `CLICK_SRC` poll:

| Poll rate | Period | Duty cycle | Bus time per second |
|---|---|---|---|
| 50 Hz | 20 ms | **0.5 %** | 5 ms |
| 100 Hz | 10 ms | **1.0 %** | 10 ms |
| 200 Hz | 5 ms | **2.0 %** | 20 ms |
| 400 Hz | 2.5 ms | **4.0 %** | 40 ms |

**Headline: ~90-100 us of bus time per 1-byte poll; polling `CLICK_SRC` at
400 Hz (matched to the ODR) costs ~4 % of the I2C bus.** That is affordable even
with the MFRC522 sharing the bus - which is the real reason to care, per
**[BADGE GUIDE]** 5 (*"The NFC chip shares this bus and can wedge it on droopy
battery power: use **bounded** I2C timeouts and retry"*) and 7 (*"Suspend accel
polling while actively scanning NFC"*).

**Software overhead is the bigger cost, not the wire.** These are wire times. An
ESP-IDF `i2c_master_transmit_receive()` round trip adds driver setup, a
semaphore wait and task scheduling - budget **150-300 us** of wall time per
1-byte read on this single-core RISC-V part, and measure it rather than trusting
the 90 us figure. At 400 Hz that is 6-12 % of one core.

---

## THE 10-MINUTE CLICK-ENGINE PROBE

Goal: an **unambiguous yes/no** on whether the SC7A20HTR's click engine works,
without an INT pin and without a scope.

**The trap to avoid:** writing `0x15` to `0x38` and reading it back proves
**nothing**. Clones routinely implement a register as plain readable/writable
storage while the logic behind it is absent. Register read-back is a *necessary*
first filter, not the test. The test must be **behavioural, with a negative
control.**

### Phase 0 - bring-up (1 min)

Confirm `WHO_AM_I (0x0F) == 0x11` (**[SC7A20 DATASHEET]** register map gives
default `00010001`; **[BADGE GUIDE]** agrees; the ESP-IDF driver and the Linux
patch both use `0x11`). Confirm the 6-byte burst from `0x28` with the
auto-increment bit gives sane gravity. If the data path is broken, stop here.

Auto-increment is **[SC7A20 DATASHEET]**-proven (10.1): *"the 7 LSb represent
the actual register address while the MSB enables address auto increment"* ->
read from `0x28 | 0x80 = 0xA8`.

### Phase 1 - register-decode filter (2 min)

For each of `0x38, 0x3A, 0x3B, 0x3C, 0x3D`: write `0x55`, read back; write
`0x2A`, read back; restore `0x00`.

| Result | Meaning |
|---|---|
| Every register reads back `0x00` no matter what you write | **Unambiguous NO.** The registers are not decoded; the engine is absent. Stop, use a software tap detector. |
| Reads come back with reserved bits masked to 0 (e.g. `0x38` returns `0x15` for `0x55`) | Registers decode **and** the reserved-bit masking matches the LIS3DH layout - encouraging. Continue. |
| Reads echo the full byte including reserved bits | Registers may be dumb RAM. Continue, but weight Phase 3 heavily. |

Also note what `0x3A` bit 7 does: if writing `0x80` sticks, `LIR_Click` plausibly
exists; if it masks to 0, assume `LIR_Click` is unimplemented and rely on
`TIME_LATENCY`.

### Phase 2 - configure (1 min)

Write in this order, with the ODR set **last** (configure while effectively idle):

```
CTRL_REG1 (0x20) = 0x00   // power down while configuring
CTRL_REG4 (0x23) = 0xA8   // BDU=1, BLE=0, FS=+-8g, HR=1 (true 12-bit)
CTRL_REG2 (0x21) = 0x04   // HPM=00, HPCF=00 (8 Hz @400Hz), FDS=0, HPCLICK=1
CTRL_REG3 (0x22) = 0x80   // I1_CLICK=1  <-- see note
CTRL_REG5 (0x24) = 0x00   // no FIFO, no latching on the AOI paths
CLICK_THS (0x3A) = 0x98   // LIR_Click=1 + 24 counts; 24 x 62.5 mg = 1.5 g
TIME_LIMIT (0x3B) = 0x14  // 20 x 2.5 ms = 50 ms  (NXP's tap time limit)
TIME_LATENCY (0x3C) = 0x28 // 40 x 2.5 ms = 100 ms  <-- keeps the event visible
TIME_WINDOW (0x3D) = 0x00 // unused for single-click
CLICK_CFG (0x38) = 0x15   // ZS | YS | XS - single click, all three axes
CTRL_REG1 (0x20) = 0x77   // 400 Hz, LPen=0, XYZ on  <-- LAST
```

Then read `CLICK_SRC (0x39)` once and discard it, to clear any boot garbage.

**Note on `CTRL_REG3 = 0x80`:** INT1 goes nowhere on this badge, so this write
is electrically pointless - **set it anyway**. Some clone implementations gate
the embedded function block on its routing-enable bit, and this costs one I2C
write. If the probe fails, re-run Phase 3 with `CTRL_REG3 = 0x00` to see whether
it made a difference; either answer is informative.

**Why these values:** ODR 400 Hz is ST's recommendation for click recognition
and NXP's "600 Hz or greater to capture the full tap signature" pushes the same
way - 400 Hz is this part's documented ceiling. +-8 g avoids the Z clipping
established in Q3. `HPCLICK=1` with `FDS=0` feeds the engine high-passed data
while your `OUT_*` registers still read gravity.

### Phase 3 - negative control, THEN positive test (4 min)

This ordering is what makes the answer unambiguous.

**3a. Negative control (90 s).** Set `CLICK_CFG = 0x00` (engine disabled, every
other register left configured). Poll `CLICK_SRC` every 2.5 ms for 30 s while
knocking the badge **hard** - harder than any gesture you intend to ship.

- `IA` (bit 6) stays 0 for the whole 30 s -> good, the bit is meaningful.
- `IA` asserts with all axes disabled -> **the bits you are reading are not a
  working click engine.** Floating/garbage register. Treat as NO.

**3b. Positive test (2.5 min).** Set `CLICK_CFG = 0x15`. Poll `CLICK_SRC` every
2.5 ms. Log every read where `src & 0x40` is set, with a timestamp. Deliver
**20 deliberate knuckle knocks** on the board edge, ~1 s apart.

Pass criteria - all four must hold:

1. **`IA` (bit 6) set** on at least 16 of 20 knocks (>=80 %).
2. **`SCLICK` (bit 4) set** on those same events - this is the single-click
   signal. (`src & 0x50 == 0x50`.)
3. **Exactly one of `Z`/`Y`/`X` (bits 2/1/0)** set per event, and the axis
   matches where you struck. The `Sign` bit (3) should flip when you knock the
   opposite face.
4. **No more than 1-2 false positives** across the 30 s of the negative control
   plus the quiet gaps between knocks.

| Outcome | Verdict |
|---|---|
| All four hold | **Unambiguous YES.** Engine works, LIS3DH layout applies. Record whether `LIR_Click` stuck. |
| `IA` fires but `SCLICK`/axis bits are static or nonsensical | **Partial.** Status encoding differs from LIS3DH. Usable as a bare "something tapped" flag; do not trust axis/sign. Treat the layout as [UNKNOWN]. |
| `IA` never fires in 3b but Phase 1 passed and 3a was clean | **Inconclusive - do Phase 4** before declaring NO. |
| `IA` fires in the negative control | **NO.** |

### Phase 4 - deterministic stimulus, no human in the loop (2 min, only if 3b was inconclusive)

Hand knocks vary; this removes them from the equation. Use **self-test** to
inject a known step, and let the high-pass filter turn the step edge into a
transient the click engine can see.

**[SC7A20 DATASHEET]** self-test output change at FS=0: *Vst1 (X) = 276 LSb,
Vst2 (Y) = 276 LSb, Vst3 (Z) = **984 LSb***. At HR +-2 g (1 mg/count) that is
276 mg on X/Y and **984 mg on Z** - so **run Phase 4 at +-2 g and use Z**.

1. `CTRL_REG4 = 0x88` (BDU, +-2 g, HR) - stay at +-2 g so self-test is largest
   relative to full scale.
2. `CLICK_THS = 0x20` (32 counts x 15.625 mg = **500 mg**, half the 984 mg Z
   self-test step), or `0xA0` if `LIR_Click` stuck in Phase 1.
3. `CLICK_CFG = 0x10` (Z single click only).
4. `TIME_LIMIT = 0x14` (50 ms), `TIME_LATENCY = 0x28` (100 ms).
5. Toggle `CTRL_REG4` bits 2:1 between `00` (normal) and `01` (self test 0) -
   i.e. alternate `0x88` and `0x8A` - with ~200 ms between toggles. Poll
   `CLICK_SRC` every 2.5 ms throughout. Do **20** toggle pairs.
6. Sanity-check first that self-test itself works: with `0x8A` held, the Z
   reading must shift by roughly +984 counts versus `0x88`. **If it does not,
   self-test is also unimplemented and Phase 4 proves nothing** - fall back to
   the Phase 3b verdict.

ST1:ST0 encoding is **[SC7A20 DATASHEET]**-proven (12.4): *"00: Normal mode;
01: Self test 0; 10: Self test 1"*.

If Z self-test produces the expected ~984-count step **and** `IA|SCLICK` fires
on the step edges, the engine is real and your knocks were simply too weak or
your `TIME_LIMIT` too short - go back to Phase 3b with `TIME_LIMIT = 0x30`
(120 ms; AN3308 Figures 17/18 show that a longer limit makes recognition much
easier) and `CLICK_THS` dropped to `0x10` at +-8 g (1 g).

If self-test works and `IA` still never fires: **unambiguous NO** - the data
path and analogue self-test are present, the click engine is not.

### Budget

| Phase | Time |
|---|---|
| 0 bring-up | 1 min |
| 1 register decode | 2 min |
| 2 configure | 1 min |
| 3a negative control | 1.5 min |
| 3b positive test | 2.5 min |
| 4 self-test (conditional) | 2 min |
| **Total** | **8-10 min** |

### Fallback if the answer is NO

Software tap detection on the polled data path, which is **[SC7A20 DATASHEET]**-
proven to work: `CTRL_REG1 = 0x77` (400 Hz), `CTRL_REG4 = 0xA8` (+-8 g, HR),
poll `STATUS (0x27)` bit 3 `ZYXDA`, burst-read 6 bytes from `0xA8`. Subtract a
slow IIR baseline (the software equivalent of the HPF), and threshold the
magnitude at ~1.5 g with a 50 ms refractory period - the same constants the
hardware engine would have used. At 400 Hz that is ~210 us of bus per sample,
~8.4 % of the bus; drop to 200 Hz (`0x67`) if that is too much alongside NFC.

---

## Quick-reference: recommended register values for this badge

| Register | Value | Purpose |
|---|---|---|
| `CTRL_REG1` `0x20` | `0x57` / `0x67` / `0x77` | 100 / 200 / 400 Hz, XYZ, normal mode |
| `CTRL_REG4` `0x23` | **`0x88`** | BDU + **HR** + +-2 g (**fixes the badge guide's `0x80`**) |
| `CTRL_REG4` `0x23` | **`0xA8`** | BDU + **HR** + +-8 g - use for tap/knock work |
| `CTRL_REG2` `0x21` | `0x04` | HPF normal, 8 Hz @400 Hz, raw data out, **HPCLICK on** |
| `CTRL_REG3` `0x22` | `0x80` | `I1_CLICK` (pin unrouted; set as insurance) |
| `CTRL_REG5` `0x24` | `0x40` | `FIFO_EN` |
| `FIFO_CTRL_REG` `0x2E` | `0x80` | Stream mode (**[INFERRED]**; only fully usable FIFO mode here) |
| `STATUS` `0x27` bit 3 | `ZYXDA` | poll for new data |
| burst read | `0x28 \| 0x80` = `0xA8` | 6 bytes XYZ, auto-increment |
| conversion | `counts = raw >> 4` | 1 mg/count @+-2 g, 4 mg @+-8 g, **8 mg @+-16 g (contested)** |

---

## Appendix: what is PROVEN vs INFERRED at a glance

| Fact | Label |
|---|---|
| `WHO_AM_I (0x0F) = 0x11` | **[SC7A20 DATASHEET]** + [BADGE GUIDE] |
| I2C auto-increment = MSB of sub-address | **[SC7A20 DATASHEET]** |
| I2C fast mode max 400 kHz | **[SC7A20 DATASHEET]** (badge runs at the spec limit) |
| CTRL_REG1 bit layout + ODR codes incl. 200/400 Hz | **[SC7A20 DATASHEET]** |
| Max characterised ODR = 400 Hz | **[SC7A20 DATASHEET]** |
| CTRL_REG2 bit layout, HPCLICK = bit 2, HPM table | **[SC7A20 DATASHEET]** |
| HPCF cutoff frequency values | [LIS3DH FAMILY, INFERRED] |
| CTRL_REG3 bit 7 = I1_CLICK | **[SC7A20 DATASHEET]** |
| CTRL_REG4 bit layout, FS bits 5:4, FS encoding, HR bit 3 | **[SC7A20 DATASHEET]** |
| Sensitivity 1/2/4/8 mg/digit HR mode | **[SC7A20 DATASHEET]** (8 at +-16 g, not 12) |
| Normal = 10-bit / LP = 8-bit, and their mg/digit | [LIS3DH FAMILY, INFERRED] |
| CTRL_REG5 bit 6 = FIFO_EN | **[SC7A20 DATASHEET]** |
| CTRL_REG6 bit 7 = I2_CLICK, bit 1 = H_LACTIVE | **[SC7A20 DATASHEET]** |
| STATUS (0x27) bit 3 = ZYXDA | **[SC7A20 DATASHEET]** + [BADGE GUIDE] |
| FIFO depth = 32 | **[SC7A20 DATASHEET]** (block diagram) |
| FIFO_CTRL_REG / FIFO_SRC_REG bit fields, FM modes | [LIS3DH FAMILY, INFERRED] |
| INT_THS LSB = FS/128 (AOI thresholds) | **[SC7A20 DATASHEET]** |
| Click engine EXISTS as a feature | **[SC7A20 DATASHEET]** (claimed, unverified) |
| CLICK_CFG/SRC/THS + TIME_* bit layouts | [LIS3DH FAMILY, INFERRED] |
| CLICK_THS LSB = FS/128 | [LIS3DH FAMILY, INFERRED] (corroborated by SC7A20's INT_THS) |
| TIME_* LSB = 1/ODR | [LIS3DH FAMILY, INFERRED] (arithmetically cross-checked) |
| CLICK_SRC bit 6 = IA, bit 4 = SCLICK | [LIS3DH FAMILY, INFERRED] |
| `LIR_Click` = CLICK_THS bit 7 | **[UNKNOWN - MUST PROBE]** |
| Whether the click engine actually fires | **[UNKNOWN - MUST PROBE]** |
| Whether FIFO mode bits decode as ST's | **[UNKNOWN - MUST PROBE]** |
| +-16 g real scale factor (8 vs 12 mg) | **[UNKNOWN - MUST PROBE]** (DS says 8, Linux says 12) |
| Non-HR data left-justification on SC7A20 | **[UNKNOWN - MUST PROBE]** |
| Behaviour of ODR codes `1000`/`1001` | **[UNKNOWN - MUST PROBE]** |
| No INT1/INT2 routed to any ESP32-C3 GPIO | **[BADGE GUIDE]** (pin map + prescribed polling) |
| GPIO8 unaccounted for in the pin map | **[BADGE GUIDE]** (worth a 60 s check) |
