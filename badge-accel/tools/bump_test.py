import math, time, serial
from serial.tools import list_ports
ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
if not ports:
    raise SystemExit("no badge port")
mags, bumps = [], []
with serial.Serial(ports[0], 115200, timeout=1) as s:
    t0 = time.monotonic()
    while time.monotonic() - t0 < 25:
        line = s.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("# BUMP"):
            bumps.append(line)
            continue
        if line.startswith("#") or line.startswith("n,"):
            continue
        p = line.split(",")
        if len(p) == 5:
            try:
                _, _, ax, ay, az = (int(v) for v in p)
                mags.append(math.sqrt(ax*ax + ay*ay + az*az))
            except ValueError:
                pass
print(f"samples: {len(mags)}")
print(f"bumps detected: {len(bumps)}")
for b in bumps[:12]:
    print("   ", b)
if mags:
    mags_sorted = sorted(mags)
    n = len(mags_sorted)
    print(f"magnitude mg -> min {mags_sorted[0]:.0f}  "
          f"median {mags_sorted[n//2]:.0f}  "
          f"p99 {mags_sorted[int(n*0.99)]:.0f}  max {mags_sorted[-1]:.0f}")
    over = sum(1 for m in mags if m >= 1600)
    print(f"samples at/over the 1600 mg threshold: {over} ({100*over/n:.2f}%)")
