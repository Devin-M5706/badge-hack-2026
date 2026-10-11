#!/usr/bin/env python3
"""Capture labelled knock candidates off the badge over USB serial.

One session per run, so a human can get into position between sessions:

    python tools/capture_knock.py knock 45
    python tools/capture_knock.py slam  45
    python tools/capture_knock.py walk  60
    python tools/capture_knock.py desk  60

Rows are appended to tools/knock_data.csv, which tools/train_knock.py reads.

WHAT A ROW IS. The firmware emits one row per ARMED CANDIDATE -- every time
the amplitude threshold was crossed and the isolation and agitation vetoes
passed, so a ringdown decision had to be made. That includes candidates the
detector then REJECTED, and those rejections are the negative class: a
classifier trained only on accepted knocks has nothing to contrast them with.

So a `slam` session is not a failure if the badge stays dark. The table slams
still arm candidates, they still get captured, and they are the most valuable
rows in the file.

The label is set on the badge before recording starts and stamped into each
row by the sampling task, not at print time, so rows cannot be mislabelled by
a session change arriving mid-flush.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial missing. install with: python -m pip install pyserial")
    sys.exit(2)

PORT = "COM3"
BAUD = 115200
OUT = Path(__file__).with_name("knock_data.csv")
HEADER = ("cand,label,peak_mg,hot,qcons,qrun,agit,axis,clipped,"
          "accepted,by_model,score")

GUIDE = {
    "knock": "Knock the badge deliberately. Vary it: face and edges, in your\n"
             "  hand and hanging on the lanyard, hard and soft. Aim for ~30.",
    "slam":  "Put the badge flat on the desk and slam the desk nearby. Do NOT\n"
             "  touch the badge. These are the rows that teach it the difference.",
    "walk":  "Clip it on and walk. Stairs if you can reach some. Do not knock it.",
    "desk":  "Leave it flat on the desk and type next to it. Do not knock it.",
}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    label = sys.argv[1].strip()[:11]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

    try:
        s = serial.Serial(PORT, BAUD, timeout=0.2)
    except Exception as exc:
        print(f"cannot open {PORT}: {exc}")
        print("A serial monitor can only be held by one process -- close it.")
        return 1

    def send(cmd: str) -> None:
        s.write((cmd + "\r").encode())
        s.flush()
        time.sleep(0.25)
        s.read(4096)          # drain the echo so it never lands in the CSV

    # Capture mode may already be on from a previous session; setting the label
    # first means no row can be written under the wrong one.
    send(f"bump label {label}")
    send("bump cap")

    print(f"\n=== {label.upper()} · {secs:.0f} s ===")
    if label in GUIDE:
        print("  " + GUIDE[label])
    print("  GO.\n")

    rows: list[str] = []
    end = time.time() + secs
    buf = ""
    last_tick = 0.0
    while time.time() < end:
        buf += s.read(2048).decode("utf-8", "replace")
        while "\r" in buf or "\n" in buf:
            sep = "\r" if "\r" in buf else "\n"
            line, buf = buf.split(sep, 1)
            line = line.strip()
            if line.startswith("cand,") and not line.startswith("cand,label"):
                rows.append(line)
        left = end - time.time()
        if left > 0 and time.time() - last_tick > 5:
            last_tick = time.time()
            print(f"  {left:4.0f}s left   captured {len(rows):3d}")

    send("bump cap off")
    s.close()

    if not rows:
        print("\n  NOTHING CAPTURED.")
        print("  For knock/slam that means the amplitude threshold was never")
        print("  crossed -- hit it harder, or lower it with `bump thr 1200`.")
        print("  For walk/desk, zero rows is a GOOD result: nothing even armed.")
        return 0

    new_file = not OUT.exists()
    with OUT.open("a", encoding="utf-8", newline="\n") as fh:
        if new_file:
            fh.write(HEADER + "\n")
        for r in rows:
            fh.write(r + "\n")

    acc = sum(1 for r in rows if r.split(",")[9] == "1")
    print(f"\n  captured {len(rows)} candidates "
          f"({acc} accepted by the current rule, {len(rows)-acc} rejected)")
    print(f"  appended to {OUT}")

    counts: dict[str, int] = {}
    if OUT.exists():
        for line in OUT.read_text(encoding="utf-8").splitlines()[1:]:
            p = line.split(",")
            if len(p) > 1:
                counts[p[1]] = counts.get(p[1], 0) + 1
    print("\n  file totals: " + "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
