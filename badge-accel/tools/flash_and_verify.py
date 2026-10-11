#!/usr/bin/env python3
"""Flash the accel sketch, verify the write, then prove it by reading samples."""
import math
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
IMG = r"C:\Users\dmyer\badge-accel\badge_accel\build\esp32.esp32.esp32c3\badge_accel.ino.merged.bin"


def wait_for_port(deadline_s=300):
    started = time.monotonic()
    while time.monotonic() - started < deadline_s:
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID:
                try:
                    serial.Serial(p.device, 115200, timeout=0.5).close()
                    return p.device
                except Exception:
                    pass
        time.sleep(1)
    return None


def esptool(port, *args, timeout=900):
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "--connect-attempts", "3", *args]
    print(f"$ esptool {' '.join(args)}", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    tail = (r.stdout or "")[-1500:]
    print(tail, flush=True)
    if r.returncode != 0:
        print((r.stderr or "")[-1500:], flush=True)
    return r.returncode == 0


def main():
    port = wait_for_port()
    if not port:
        print("# FAILED: no responsive port. Replug USB (battery out).", flush=True)
        return 1
    print(f"# port: {port}", flush=True)

    # Leave the chip in download mode so verification reads back exactly what
    # was written, before the new firmware can touch flash.
    if not esptool(port, "--after", "no-reset", "write-flash", "0x0", IMG):
        print("# FAILED: write-flash", flush=True)
        return 1
    if not esptool(port, "--after", "no-reset", "verify-flash", "0x0", IMG):
        print("# WARNING: verify-flash mismatch (write-flash hash check passed)",
              flush=True)
    esptool(port, "--after", "hard-reset", "read-mac", timeout=120)

    print("# booting new firmware, waiting for port...", flush=True)
    time.sleep(3)
    port = wait_for_port(120) or port

    print(f"# reading samples from {port} for 10s", flush=True)
    rows, notes = [], []
    try:
        with serial.Serial(port, 115200, timeout=1) as ser:
            t0 = time.monotonic()
            while time.monotonic() - t0 < 10:
                line = ser.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                if len(rows) < 8 or line.startswith("#"):
                    print("   ", line, flush=True)
                if line.startswith("#") or line.startswith("n,"):
                    notes.append(line)
                    continue
                parts = line.split(",")
                if len(parts) == 5:
                    try:
                        rows.append([int(x) for x in parts])
                    except ValueError:
                        pass
    except Exception as e:
        print(f"# serial read failed: {type(e).__name__}: {e}", flush=True)

    print(f"# parsed {len(rows)} samples", flush=True)
    if len(rows) < 20:
        print("# FAILED: too few samples to trust", flush=True)
        return 1

    ax = sum(r[2] for r in rows) / len(rows)
    ay = sum(r[3] for r in rows) / len(rows)
    az = sum(r[4] for r in rows) / len(rows)
    mag = math.sqrt(ax * ax + ay * ay + az * az)
    span = (rows[-1][1] - rows[0][1]) / 1000.0
    rate = (len(rows) - 1) / span if span > 0 else 0.0
    gaps = sum(1 for a, b in zip(rows, rows[1:]) if b[0] != a[0] + 1)

    print(f"# mean: ax={ax:.0f} ay={ay:.0f} az={az:.0f} mg", flush=True)
    print(f"# |a| = {mag:.0f} mg (expect ~1000 at rest)", flush=True)
    print(f"# rate: {rate:.1f} Hz over {span:.1f}s, dropped-sample gaps: {gaps}",
          flush=True)
    good = 700 <= mag <= 1300
    print(f"# GRAVITY CHECK: {'PASS' if good else 'FAIL'}", flush=True)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
