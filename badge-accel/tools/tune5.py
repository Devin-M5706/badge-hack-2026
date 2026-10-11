import json, re, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]

def cmd(s, c, wait=0.7, show=True):
    s.write(c.encode() + b"\r"); s.flush(); time.sleep(wait)
    out = s.read(4000).decode("utf-8", "replace").replace("\r", "\n")
    if show:
        for ln in out.splitlines():
            if ln.strip(): print(f"   {ln.strip()}", flush=True)
    return out

def stats(s):
    for ln in cmd(s, "s", wait=1.0, show=False).splitlines():
        if ln.strip().startswith("ST "):
            return ln.strip()
    return ""

def f(line, key):
    m = re.search(rf"\b{key}=(-?\d+)", line)
    return int(m.group(1)) if m else None

buf, events = b"", []
with serial.Serial(dev, 115200, timeout=1) as s:
    s.reset_input_buffer()
    print("--- re-applying the full set (NVS was wiped by the reflash) ---", flush=True)
    for c in ("p1", "n20", "h20", "a0", "d1"):
        cmd(s, c)
    cmd(s, "S")
    cmd(s, "z")
    time.sleep(1)
    before = stats(s)
    print(f"BEFORE {before}", flush=True)
    print("\n### BUMP IT - 60s ###", flush=True)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 60:
        c = s.read(256)
        if not c: continue
        buf += c
        while b"\r" in buf:
            ln, buf = buf.split(b"\r", 1)
            t = ln.decode("utf-8", "replace").strip()
            if t and t[0] == "{":
                events.append(t); print("   EVENT", t, flush=True)
    after = stats(s)
    print(f"\nAFTER  {after}", flush=True)

acc = (f(after, "ev") or 0) - (f(before, "ev") or 0)
rq = (f(after,"rq") or 0) - (f(before,"rq") or 0)
ra = (f(after,"ra") or 0) - (f(before,"ra") or 0)
rd = (f(after,"rd") or 0) - (f(before,"rd") or 0)
tot = acc + rq + ra + rd
print(f"\naccepted {acc} / {tot} candidates" + (f"  ({100*acc//tot}%)" if tot else ""))
print(f"rejected: isolation {rq}  agitation {ra}  ringdown {rd}")
print(f"NDJSON captured {len(events)}")
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    print(f"mg range {min(mgs)}-{max(mgs)}")
print(f"hz {f(before,'hz')} -> {f(after,'hz')}   worst gap us "
      f"{f(before,'gap')} -> {f(after,'gap')}   i2c err {f(after,'i2e')}")
