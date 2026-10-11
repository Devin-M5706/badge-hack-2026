"""Measure the loop rate while NO host is reading the port.

The badge's own millis() clock is the reference, so closing the port (which
also resets the board) does not invalidate the measurement: we compare the
sample counter against the badge clock across the detached window.
"""
import time, serial
from serial.tools import list_ports

def snapshot(label):
    dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]
    with serial.Serial(dev, 115200, timeout=2) as s:
        s.reset_input_buffer()
        for _ in range(60):
            line = s.readline().decode("utf-8", "replace").strip()
            p = line.split(",")
            if len(p) == 5 and not line.startswith("#"):
                try:
                    n, t = int(p[0]), int(p[1])
                    print(f"{label}: n={n} t_ms={t}")
                    return n, t
                except ValueError:
                    pass
    raise SystemExit(f"{label}: no sample")

n1, t1 = snapshot("before")
print("port closed; badge unattended for 15s...")
time.sleep(15)
n2, t2 = snapshot("after ")

dn, dt = n2 - n1, (t2 - t1) / 1000.0
if dt <= 0:
    print(f"\nbadge rebooted on close (uptime now {t2/1000:.1f}s)")
    print(f"detached rate = {n2/(t2/1000.0):.1f} Hz over its {t2/1000:.1f}s of life")
    rate = n2 / (t2 / 1000.0)
else:
    rate = dn / dt
    print(f"\nelapsed {dt:.1f}s, {dn} samples -> {rate:.1f} Hz")
print("VERDICT:", "FIXED - runs standalone" if rate > 70 else "STILL STALLING")
