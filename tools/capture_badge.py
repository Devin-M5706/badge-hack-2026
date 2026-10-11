#!/usr/bin/env python3
"""Capture a labelled session from the badge-accel firmware and extract
candidate features for tools/train_knock.py.

    python tools/capture_badge.py knock 45
    python tools/capture_badge.py slam  45
    python tools/capture_badge.py walk  45

WHY THIS EXISTS SEPARATELY FROM capture_knock.py
------------------------------------------------
capture_knock.py talks to firmware/ (the PlatformIO build), which emits one
pre-computed candidate row per armed candidate via `bump cap`. The badge is
running badge-accel instead -- the demo firmware -- which has no such command.
What it does have is better for this purpose: a raw 100 Hz CSV stream of
ax,ay,az in milligravities.

So this captures the raw stream and computes the candidate features on the
host, using the same bump_logic mirror the SisFall replay uses. Nothing is
flashed. The demo firmware is untouched.

HONEST CAVEATS, because they affect what the trained weights mean
----------------------------------------------------------------
  * badge-accel samples at 100 Hz; the detector's timings are defined at
    400 Hz. Samples are upsampled 4x so a window of N samples spans the
    duration it would on the 400 Hz build. Interpolation cannot invent the
    content a real 400 Hz capture would carry, so ringdown reads softer here.
  * badge-accel runs the sensor at +/-2 g, so each axis clips at ~2048 mg. A
    hard knock saturates per-axis; the vector magnitude still rises because
    three axes sum, but the peak feature is compressed at the top end.
  * Both of the above are properties of the demo firmware, not of the
    detector. firmware/ runs 400 Hz at +/-8 g and would not have either.

Labels: anything containing "knock" becomes the positive class in
train_knock.py. Everything else is a negative.
"""

from __future__ import annotations

import importlib.util
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial missing: python -m pip install pyserial")
    sys.exit(2)

HERE = Path(__file__).parent
PORT = "COM3"
BAUD = 115200
OUT = HERE / "knock_data.csv"
RAW_DIR = HERE / "raw"
HEADER = ("cand,label,peak_mg,hot,qcons,qrun,agit,axis,clipped,"
          "accepted,by_model,score")

GUIDE = {
    "knock": "KNOCK IT deliberately, about 30 times. Vary it: face, edges, in\n"
             "  your hand, hanging on the lanyard, hard and soft.",
    "slam":  "Badge FLAT ON THE DESK. Slam the desk near it. DO NOT touch the\n"
             "  badge. These are the negatives that teach it the difference.",
    "walk":  "Clip it on and walk. Do not knock it.",
    "desk":  "Leave it flat and type next to it. Do not knock it.",
}


def load_mirror():
    """Import the detector mirror from the replay harness."""
    spec = importlib.util.spec_from_file_location("rs", HERE / "replay_sisfall.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["rs"] = mod          # dataclasses need the module registered
    spec.loader.exec_module(mod)
    return mod


def upsample_4x(rows: list[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    """100 Hz -> 400 Hz, three interpolated points between each real pair."""
    if len(rows) < 2:
        return list(rows)
    out: list[tuple[int, int, int]] = []
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        out.append(a)
        for k in (1, 2, 3):
            out.append(tuple(a[j] + (b[j] - a[j]) * k // 4 for j in range(3)))
    out.append(rows[-1])
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    label = sys.argv[1].strip()[:16]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

    try:
        s = serial.Serial(PORT, BAUD, timeout=0.2)
    except Exception as exc:
        print(f"cannot open {PORT}: {exc}")
        print("Only one process can hold the port -- close any serial monitor.")
        return 1

    print(f"\n=== {label.upper()} - {secs:.0f}s ===")
    if label in GUIDE:
        print("  " + GUIDE[label])
    print("  GO.\n")

    rows: list[tuple[int, int, int]] = []
    bumps = 0
    buf = ""
    end = time.time() + secs
    tick = 0.0
    s.reset_input_buffer()
    while time.time() < end:
        buf += s.read(4096).decode("utf-8", "replace")
        while "\n" in buf or "\r" in buf:
            sep = "\n" if "\n" in buf else "\r"
            line, buf = buf.split(sep, 1)
            line = line.strip()
            if not line:
                continue
            if line.startswith("# BUMP"):
                bumps += 1
                continue
            if line.startswith("#") or line.startswith("n,"):
                continue
            p = line.split(",")
            if len(p) < 5:
                continue
            try:
                rows.append((int(p[2]), int(p[3]), int(p[4])))
            except ValueError:
                continue
        left = end - time.time()
        if left > 0 and time.time() - tick > 5:
            tick = time.time()
            print(f"  {left:4.0f}s left   samples {len(rows):5d}   "
                  f"firmware bumps {bumps}")
    s.close()

    print(f"\n  captured {len(rows)} samples at ~100 Hz "
          f"({len(rows)/100.0:.1f}s), firmware reported {bumps} bumps")
    if len(rows) < 400:
        print("  TOO FEW SAMPLES. Is the badge streaming? Open the monitor and")
        print("  check you see lines like '123,4567,14,177,1065'.")
        return 1

    RAW_DIR.mkdir(exist_ok=True)
    raw_path = RAW_DIR / f"{label}.csv"
    with raw_path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("ax_mg,ay_mg,az_mg\n")
        for r in rows:
            fh.write(f"{r[0]},{r[1]},{r[2]}\n")
    print(f"  raw -> {raw_path}")

    # ---- extract candidates with the detector mirror -----------------------
    rs = load_mirror()
    up = upsample_4x(rows)
    det = rs.Detector(rs.Config())
    cands = []
    for i, (x, y, z) in enumerate(up):
        det.last_cand = None
        det.feed(x, y, z, i)
        if det.last_cand is not None:
            cands.append(det.last_cand)

    if not cands:
        print("\n  NO CANDIDATES ARMED.")
        print("  The amplitude threshold (1500 mg of AC magnitude) was never")
        print("  crossed with enough preceding quiet. For a knock session that")
        print("  means hit it harder; for walk/desk, zero is the right answer.")
        return 0

    acc = sum(1 for c in cands if c["accepted"])
    new_file = not OUT.exists()
    with OUT.open("a", encoding="utf-8", newline="\n") as fh:
        if new_file:
            fh.write(HEADER + "\n")
        for c in cands:
            fh.write(f"cand,{label},{c['peak_mg']},{c['hot']},{c['qcons']},"
                     f"{c['qrun']},{c['agit']},{c['axis']},{c['clipped']},"
                     f"{c['accepted']},0,0\n")

    print(f"\n  {len(cands)} candidates extracted "
          f"({acc} the hand-tuned rule would accept, {len(cands)-acc} it rejects)")
    print(f"  appended to {OUT}")

    counts: dict[str, int] = {}
    for line in OUT.read_text(encoding="utf-8").splitlines()[1:]:
        p = line.split(",")
        if len(p) > 1:
            counts[p[1]] = counts.get(p[1], 0) + 1
    print("  file totals: " + "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
