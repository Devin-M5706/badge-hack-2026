"""Five-minute open capture. Bump whenever; it reports as events arrive."""
import json, re, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]
def stats(s):
    s.write(b"s\r"); s.flush(); time.sleep(1.0)
    for ln in s.read(4000).decode("utf-8","replace").replace("\r","\n").splitlines():
        if ln.strip().startswith("ST "): return ln.strip()
    return ""
def f(l, k):
    m = re.search(rf"\b{k}=(-?\d+)", l); return int(m.group(1)) if m else 0
buf, events = b"", []
with serial.Serial(dev, 115200, timeout=1) as s:
    s.reset_input_buffer(); time.sleep(0.3)
    before = stats(s)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 300:
        c = s.read(256)
        if not c: continue
        buf += c
        while b"\r" in buf:
            ln, buf = buf.split(b"\r", 1)
            t = ln.decode("utf-8","replace").strip()
            if t and t[0] == "{":
                events.append(t)
                print(f"[{time.monotonic()-t0:5.1f}s] {t}", flush=True)
    after = stats(s)
acc = f(after,"ev")-f(before,"ev")
rq, ra, rd = (f(after,k)-f(before,k) for k in ("rq","ra","rd"))
tot = acc+rq+ra+rd
print(f"\naccepted {acc} / {tot}" + (f"  ({100*acc//tot}%)" if tot else "  (no candidates)"))
print(f"rejected: isolation {rq}  agitation {ra}  ringdown {rd}")
if events:
    ev = [json.loads(e) for e in events]
    print(f"mg {min(x['mg'] for x in ev)}-{max(x['mg'] for x in ev)}")
    print(f"flags seen: {sorted({x['flags'] for x in ev})}  (0=clean 2=WEAK 4=UNVETTED)")
print(f"i2c err {f(after,'i2e')}  worst gap us {f(after,'gap')}")
