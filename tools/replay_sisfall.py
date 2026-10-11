#!/usr/bin/env python3
"""Replay the SisFall dataset through the knock detector's logic.

WHAT THIS IS FOR
----------------
There is no labelled dataset of knocks on this badge, so the detector's
thresholds cannot be learned and its false-positive rate cannot be measured
against anything but one person's afternoon. SisFall is the closest public
substitute: 19 activities of daily living and 15 fall types from 38
participants, recorded on a waist-mounted two-accelerometer device at 200 Hz.
Falls are impact transients against a noisy body-mounted baseline, which is
structurally the problem this firmware solves.

So this script answers one question with real human motion instead of
synthetic waveforms:

    How many knock events does the detector emit across hours of ordinary
    walking, sitting, stairs and jogging that contain no knocks at all?

Every one of those is a false positive, and the count is a number worth
printing in a README.

WHAT IT CANNOT ANSWER
---------------------
  * It is not knock detection. Nobody knocked on anything in SisFall. A fall
    firing the detector shows the impulse path responds to real impacts, not
    that it recognises a knuckle strike.
  * The mounting is wrong. SisFall is waist-mounted; the badge hangs on a
    lanyard or sits in a hand. Coupling, damping and ringdown all differ.
  * The rate is half. SisFall samples at 200 Hz; the firmware runs at 400 Hz.
    We upsample so the detector's per-sample timings mean what they mean on
    hardware -- but interpolation CANNOT invent the >100 Hz content a real
    400 Hz capture would carry. Ringdown behaviour is therefore softer here
    than on the badge, which biases this toward *fewer* false positives.

So read the output as a floor on false-positive behaviour under real motion,
not as a validation of knock recognition.

WHY PYTHON, AND HOW IT STAYS HONEST
-----------------------------------
The authoritative detector is C++ (firmware/lib/BumpLogic). This machine has
no host C++ compiler, so that code cannot run here. The Detector class below
is a deliberate line-by-line mirror of bump_logic.cpp, in integer arithmetic
only -- Python's `>>` on negatives floors exactly like a GCC arithmetic shift,
and every intermediate stays inside int32 range, so the two agree bit for bit.

That claim is not left to trust: `--emit-fixture` writes the exact sample
stream and the decisions this mirror produced, and
firmware/test/test_sisfall_replay/ replays that same fixture through the real
bump::Detector in CI and fails if a single event differs. If the mirror ever
drifts, the build breaks.

USAGE
-----
    python tools/replay_sisfall.py --data <dir containing SisFall .txt files>
    python tools/replay_sisfall.py --data <dir> --sensor adxl345
    python tools/replay_sisfall.py --data <dir> --emit-fixture fixture.h --limit 40
    python tools/replay_sisfall.py --data <dir> --json results.json

SisFall is CC BY 4.0. Sucerquia, A.; Lopez, J. D.; Vargas-Bonilla, J. F.
"SisFall: A Fall and Movement Dataset." Sensors 2017, 17(1), 198.
https://doi.org/10.3390/s17010198
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field, asdict
from pathlib import Path

# ---------------------------------------------------------------------------
# Mirror of firmware/lib/BumpLogic/bump_logic.{h,cpp}
# Keep these in lockstep with that header. The CI fixture test is what
# enforces it.
# ---------------------------------------------------------------------------

K_EMA_SHIFT = 7
K_EMA_FRAC = 8          # MUST be >= K_EMA_SHIFT or the tracker stalls at zero
K_WARM_ITERS = 320
K_RAIL_MG = 8160        # 2040 counts x 4 mg: the +/-8 g rail
K_MS_PER_ITER = 2

FLAG_CLIPPED = 0x01
FLAG_WEAK = 0x02
FLAG_UNVETTED = 0x04


@dataclass
class Config:
    thr_mg: int = 1500
    qgate_mg: int = 240
    qneed: int = 80
    qpen: int = 8
    dshift: int = 2
    dwin: int = 24
    dhot_max: int = 8
    dq_need: int = 4
    refr: int = 100
    rlock: int = 40
    agit_max: int = 600
    agit_en: int = 1


@dataclass
class Stats:
    samples: int = 0
    events: int = 0
    rej_quiet: int = 0
    rej_agit: int = 0
    rej_decay: int = 0
    mag_max: int = 0


@dataclass
class Event:
    index: int
    peak_mg: int
    axis: int
    flags: int
    seq: int


class Detector:
    """Line-by-line mirror of bump::Detector. Integer arithmetic only."""

    def __init__(self, cfg: Config | None = None) -> None:
        self.cfg = cfg or Config()
        self.st = Stats()
        self.reset()

    def reset(self) -> None:
        self.ema = [0, 0, 0]
        self.qrun = 0
        self.agit = 0
        self.warm = K_WARM_ITERS
        self.lock = 0
        self.dn = 0
        self.peak = 0
        self.hot = 0
        self.qcons = 0
        self.pk_axis = 2
        self.in_decay = False
        self.pk_clipped = False

    def feed(self, x_mg: int, y_mg: int, z_mg: int, index: int = 0) -> Event | None:
        cfg = self.cfg
        raw = (x_mg, y_mg, z_mg)
        clipped = any(v >= K_RAIL_MG or v <= -K_RAIL_MG for v in raw)

        mag = 0
        amax = -1
        ax = 2
        for i in range(3):
            xq = raw[i] << K_EMA_FRAC
            if self.warm == K_WARM_ITERS:
                self.ema[i] = xq
            self.ema[i] += (xq - self.ema[i]) >> K_EMA_SHIFT
            ac = abs(raw[i] - (self.ema[i] >> K_EMA_FRAC))
            mag += ac
            if ac > amax:
                amax = ac
                ax = i

        self.st.samples += 1
        if mag > self.st.mag_max:
            self.st.mag_max = mag

        if mag < cfg.qgate_mg:
            if self.qrun < 8192:
                self.qrun += 1
            if self.agit > 0:
                self.agit -= 1
        else:
            self.qrun -= cfg.qpen
            if self.qrun < 0:
                self.qrun = 0
            self.agit += 8
            if self.agit > 8192:
                self.agit = 8192

        if self.warm > 0:
            self.warm -= 1
            return None

        if self.lock > 0:
            self.lock -= 1
            if self.lock == 0:
                self.qrun = 0
            return None

        if not self.in_decay:
            if mag < cfg.thr_mg:
                return None
            if self.qrun < cfg.qneed:
                self.st.rej_quiet += 1
                self.lock = cfg.rlock
                return None
            if cfg.agit_en != 0 and self.agit > cfg.agit_max:
                self.st.rej_agit += 1
                self.lock = cfg.rlock
                return None
            self.peak = mag
            self.pk_axis = ax
            self.pk_clipped = clipped
            self.hot = 0
            self.qcons = 0
            self.dn = cfg.dwin
            self.in_decay = True
            return None

        if mag > self.peak:
            self.peak = mag
            self.pk_axis = ax
        if clipped:
            self.pk_clipped = True

        if mag >= (cfg.thr_mg >> cfg.dshift):
            self.hot += 1
            self.qcons = 0
        else:
            self.qcons += 1

        self.dn -= 1
        if self.dn > 0:
            return None

        self.in_decay = False
        self.lock = cfg.refr

        if self.hot > cfg.dhot_max or self.qcons < cfg.dq_need:
            self.st.rej_decay += 1
            return None

        self.st.events += 1
        flags = 0
        if self.pk_clipped:
            flags |= FLAG_CLIPPED
        if self.peak < cfg.thr_mg + (cfg.thr_mg >> 1):
            flags |= FLAG_WEAK
        if cfg.agit_en == 0:
            flags |= FLAG_UNVETTED
        return Event(index=index, peak_mg=self.peak, axis=self.pk_axis,
                     flags=flags, seq=self.st.events)


# ---------------------------------------------------------------------------
# SisFall loading
# ---------------------------------------------------------------------------

# Column layout per the dataset's own README: nine comma-separated integers,
# ADXL345 x,y,z then ITG3200 x,y,z then MMA8451Q x,y,z. Conversion to g is
# [(2 * Range) / 2^Resolution] * AD.
#
# MMA8451Q is the default here because its +/-8 g range matches the badge's
# CTRL_REG4 = 0xA8 configuration exactly; ADXL345 is +/-16 g, so a given
# impact lands on a coarser grid.
SENSORS = {
    "mma8451q": {"cols": (6, 7, 8), "range_g": 8,  "bits": 14},
    "adxl345":  {"cols": (0, 1, 2), "range_g": 16, "bits": 13},
}

SISFALL_HZ = 200
TARGET_HZ = 400

# D01..D19 are activities of daily living, F01..F15 are falls.
FNAME = re.compile(r"^([DF])(\d+)_(SA|SE)(\d+)_R(\d+)\.txt$", re.IGNORECASE)

# Sustained locomotion, from the dataset's own activity table. These stress
# the amplitude veto: the design claims gait is rejected on amplitude rather
# than isolation, and D01-D06 are where that claim either holds or doesn't.
LOCOMOTION = {
    "D01",  # walking slowly
    "D02",  # walking quickly
    "D03",  # jogging slowly
    "D04",  # jogging quickly
    "D05",  # stairs, slowly
    "D06",  # stairs, quickly
}

# The genuinely hard false positives: ADLs that contain a real impulse. These
# are near-fall events a knock detector has no principled way to tell from a
# deliberate strike, and D18/D19 in particular are the cases I expect to cost
# us. Tracked separately so they do not hide inside the ADL average.
IMPULSIVE_ADL = {
    "D11",  # trying to get up and collapsing into a chair
    "D13",  # sitting, then lying down quickly
    "D18",  # stumble while walking
    "D19",  # gently jump to reach a high object
}

ACTIVITY_NAMES = {
    "D01": "walking slowly", "D02": "walking quickly",
    "D03": "jogging slowly", "D04": "jogging quickly",
    "D05": "stairs slowly", "D06": "stairs quickly",
    "D07": "sit half-height, slow", "D08": "sit half-height, quick",
    "D09": "sit low chair, slow", "D10": "sit low chair, quick",
    "D11": "collapse into chair", "D12": "lie down slowly",
    "D13": "lie down quickly", "D14": "roll over supine",
    "D15": "bend at knees", "D16": "bend without knees",
    "D17": "get in/out of a car", "D18": "stumble while walking",
    "D19": "gentle jump",
}


@dataclass
class FileResult:
    name: str
    kind: str          # "adl" or "fall"
    activity: str      # D04, F01, ...
    subject: str
    samples: int
    seconds: float
    events: int
    rej_quiet: int
    rej_agit: int
    rej_decay: int
    mag_max: int
    peaks: list = field(default_factory=list)


def to_mg(ad: int, range_g: int, bits: int) -> int:
    """Dataset AD counts -> integer milligravities, matching the README formula."""
    # (2*range / 2^bits) * AD  in g  ->  *1000 for mg. Done in integers so the
    # mirror stays deterministic across platforms.
    return (ad * 2 * range_g * 1000) // (1 << bits)


def upsample_2x(rows: list[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    """200 Hz -> 400 Hz by inserting one midpoint between each pair.

    This makes an iteration of the detector mean 2.5 ms, as it does on the
    badge, so qneed/dwin/refr keep their designed durations. It does not and
    cannot add spectral content above the original Nyquist limit -- stated in
    the module docstring as the main caveat of this whole experiment.
    """
    if len(rows) < 2:
        return list(rows)
    out: list[tuple[int, int, int]] = []
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        out.append(a)
        out.append(((a[0] + b[0]) // 2, (a[1] + b[1]) // 2, (a[2] + b[2]) // 2))
    out.append(rows[-1])
    return out


def load_file(path: Path, sensor: str) -> list[tuple[int, int, int]]:
    spec = SENSORS[sensor]
    cx, cy, cz = spec["cols"]
    rg, bits = spec["range_g"], spec["bits"]
    rows: list[tuple[int, int, int]] = []
    with path.open("r", errors="replace") as fh:
        for line in fh:
            line = line.strip().rstrip(";")
            if not line:
                continue
            parts = line.split(",")
            if len(parts) < 9:
                continue
            try:
                vals = [int(p.strip()) for p in parts[:9]]
            except ValueError:
                continue
            rows.append((to_mg(vals[cx], rg, bits),
                         to_mg(vals[cy], rg, bits),
                         to_mg(vals[cz], rg, bits)))
    return rows


def run_file(path: Path, sensor: str, cfg: Config) -> FileResult | None:
    m = FNAME.match(path.name)
    if not m:
        return None
    activity = (m.group(1) + m.group(2)).upper()
    kind = "adl" if m.group(1).upper() == "D" else "fall"
    subject = (m.group(3) + m.group(4)).upper()

    rows = load_file(path, sensor)
    if len(rows) < K_WARM_ITERS:
        return None
    rows = upsample_2x(rows)

    det = Detector(Config(**asdict(cfg)))
    peaks = []
    for i, (x, y, z) in enumerate(rows):
        ev = det.feed(x, y, z, i)
        if ev is not None:
            peaks.append(ev.peak_mg)

    return FileResult(
        name=path.name, kind=kind, activity=activity, subject=subject,
        samples=len(rows), seconds=len(rows) / TARGET_HZ,
        events=det.st.events, rej_quiet=det.st.rej_quiet,
        rej_agit=det.st.rej_agit, rej_decay=det.st.rej_decay,
        mag_max=det.st.mag_max, peaks=peaks,
    )


# ---------------------------------------------------------------------------
# Fixture emission for the C++ equivalence test
# ---------------------------------------------------------------------------

def emit_fixture(results_paths: list[Path], sensor: str, cfg: Config,
                 out: Path, limit: int) -> None:
    """Write a C header holding sample streams plus this mirror's decisions.

    firmware/test/test_sisfall_replay replays these through the real
    bump::Detector and asserts the event sequences match exactly. That test is
    the only thing standing between "the Python mirrors the firmware" and
    wishful thinking.
    """
    chosen = results_paths[:limit]
    blocks = []
    for path in chosen:
        m = FNAME.match(path.name)
        if not m:
            continue
        rows = load_file(path, sensor)
        if len(rows) < K_WARM_ITERS:
            continue
        rows = upsample_2x(rows)
        det = Detector(Config(**asdict(cfg)))
        evs = []
        for i, (x, y, z) in enumerate(rows):
            ev = det.feed(x, y, z, i)
            if ev is not None:
                evs.append(ev)
        blocks.append((path.name, rows, evs))

    with out.open("w", newline="\n") as fh:
        fh.write("// GENERATED by tools/replay_sisfall.py -- do not edit.\n")
        fh.write("//\n")
        fh.write("// Sample streams from SisFall (CC BY 4.0, doi:10.3390/s17010198)\n")
        fh.write("// converted to milligravities and upsampled to 400 Hz, paired with\n")
        fh.write("// the decisions the Python mirror produced. The native test replays\n")
        fh.write("// these through the real bump::Detector; any divergence fails CI.\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write("typedef struct {\n"
                 "  const char*    name;\n"
                 "  const int16_t* xyz;     /* 3 per sample, interleaved */\n"
                 "  uint32_t       n;\n"
                 "  const int32_t* peaks;   /* expected peak_mg, in order */\n"
                 "  uint32_t       n_events;\n"
                 "} sisfall_case_t;\n\n")
        for idx, (name, rows, evs) in enumerate(blocks):
            fh.write(f"/* {name}: {len(rows)} samples, {len(evs)} events */\n")
            fh.write(f"static const int16_t sf_xyz_{idx}[] = {{\n")
            for j in range(0, len(rows), 8):
                chunk = rows[j:j + 8]
                fh.write("  " + ", ".join(f"{v}" for r in chunk for v in r) + ",\n")
            fh.write("};\n")
            if evs:
                fh.write(f"static const int32_t sf_pk_{idx}[] = {{"
                         + ", ".join(str(e.peak_mg) for e in evs) + "};\n\n")
            else:
                fh.write(f"static const int32_t sf_pk_{idx}[] = {{0}};\n\n")
        fh.write("static const sisfall_case_t SISFALL_CASES[] = {\n")
        for idx, (name, rows, evs) in enumerate(blocks):
            fh.write(f'  {{ "{name}", sf_xyz_{idx}, {len(rows)}, '
                     f"sf_pk_{idx}, {len(evs)} }},\n")
        fh.write("};\n")
        fh.write(f"static const uint32_t SISFALL_CASE_COUNT = {len(blocks)};\n")
    print(f"fixture: {out}  ({len(blocks)} cases)")


# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, type=Path,
                    help="directory holding SisFall .txt files (searched recursively)")
    ap.add_argument("--sensor", default="mma8451q", choices=sorted(SENSORS),
                    help="which accelerometer to replay (default mma8451q, +/-8 g like the badge)")
    ap.add_argument("--thr", type=int, default=None, help="override threshold, mg")
    ap.add_argument("--json", type=Path, default=None, help="write per-file results here")
    ap.add_argument("--emit-fixture", type=Path, default=None,
                    help="write a C header fixture for the CI equivalence test")
    ap.add_argument("--limit", type=int, default=0, help="only replay the first N files")
    ap.add_argument("--fixture-cases", type=int, default=24,
                    help="how many files go into the fixture (keep the header small)")
    args = ap.parse_args()

    if not args.data.is_dir():
        print(f"not a directory: {args.data}", file=sys.stderr)
        return 2

    files = sorted(p for p in args.data.rglob("*.txt") if FNAME.match(p.name))
    if not files:
        print(f"no SisFall-shaped .txt files under {args.data}", file=sys.stderr)
        print("expected names like D04_SA01_R01.txt", file=sys.stderr)
        return 2
    if args.limit:
        files = files[:args.limit]

    cfg = Config()
    if args.thr is not None:
        cfg.thr_mg = args.thr

    print(f"replaying {len(files)} files | sensor={args.sensor} "
          f"| thr={cfg.thr_mg} mg | {SISFALL_HZ}->{TARGET_HZ} Hz")

    results: list[FileResult] = []
    for n, path in enumerate(files, 1):
        r = run_file(path, args.sensor, cfg)
        if r is not None:
            results.append(r)
        if n % 200 == 0:
            print(f"  ... {n}/{len(files)}")

    adl = [r for r in results if r.kind == "adl"]
    fall = [r for r in results if r.kind == "fall"]
    loco = [r for r in adl if r.activity in LOCOMOTION]

    def total(rs, attr):
        return sum(getattr(r, attr) for r in rs)

    def secs(rs):
        return sum(r.seconds for r in rs)

    print("\n" + "=" * 68)
    print("FALSE POSITIVES ON REAL HUMAN MOTION  (no knocks exist in this data)")
    print("=" * 68)
    impulsive = [r for r in adl if r.activity in IMPULSIVE_ADL]
    for label, rs in (("all ADLs", adl), ("locomotion", loco),
                      ("impulsive ADLs", impulsive)):
        if not rs:
            continue
        s, e = secs(rs), total(rs, "events")
        rate = (e / (s / 60.0)) if s else 0.0
        print(f"{label:>16}: {e:>5} events over {s/60:>7.1f} min "
              f"({len(rs)} files)  ->  {rate:.3f} FP/min")
    print(f"\n{'vetoed by isolation':>22}: {total(adl,'rej_quiet'):>6}")
    print(f"{'vetoed by agitation':>22}: {total(adl,'rej_agit'):>6}")
    print(f"{'vetoed by ringdown':>22}: {total(adl,'rej_decay'):>6}")

    if fall:
        hit = sum(1 for r in fall if r.events > 0)
        rate = 100.0 * hit / len(fall)
        print("\n" + "=" * 68)
        print("RESPONSE TO REAL IMPACTS  (falls -- NOT knocks)")
        print("=" * 68)
        print(f"  {hit}/{len(fall)} fall recordings produced at least one event "
              f"({rate:.1f}%)")
        # Say what the number means rather than what we hoped it would mean. A
        # fall is a violent, long, multi-lobe event; a knock is one short lobe.
        # The ringdown test exists to separate exactly those, so a LOW number
        # here is the discriminator generalising, not the detector being dead --
        # provided the detector demonstrably fires on real knocks on hardware.
        # That second half cannot come from this dataset: nobody tapped
        # anything in SisFall.
        if rate < 5.0:
            print("  Reads as: the ringdown test rejects violent multi-lobe impacts,")
            print("  which is what it was built to do. This is only good news in")
            print("  combination with a bench run showing real knocks DO fire --")
            print("  this dataset contains no taps and cannot show acceptance.")
        elif rate > 60.0:
            print("  Reads as: the impulse path fires on violent impacts too. For a")
            print("  knock detector that is a false-positive source, not a feature.")
        else:
            print("  Mixed. Inspect the ringdown counters before drawing a conclusion.")

    worst = sorted(adl, key=lambda r: r.events, reverse=True)[:8]
    if worst and worst[0].events:
        print("\nWorst ADL files (where to look first):")
        for r in worst:
            if not r.events:
                break
            print(f"  {r.name:<22} {r.activity}  {r.events} event(s)  "
                  f"peaks={r.peaks[:4]} mag_max={r.mag_max}")

    by_act = defaultdict(lambda: [0, 0.0])
    for r in adl:
        by_act[r.activity][0] += r.events
        by_act[r.activity][1] += r.seconds
    noisy = sorted(by_act.items(), key=lambda kv: -kv[1][0])[:6]
    if noisy and noisy[0][1][0]:
        print("\nBy activity:")
        for act, (e, s) in noisy:
            if not e:
                break
            print(f"  {act}  {e:>4} events / {s/60:>6.1f} min")

    if args.json:
        args.json.write_text(json.dumps(
            {"config": asdict(cfg), "sensor": args.sensor,
             "files": [asdict(r) for r in results]}, indent=1))
        print(f"\njson: {args.json}")

    if args.emit_fixture:
        emit_fixture(files, args.sensor, cfg, args.emit_fixture, args.fixture_cases)

    return 0


if __name__ == "__main__":
    sys.exit(main())
