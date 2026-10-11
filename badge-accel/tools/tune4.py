import json, re, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]

def cmd(s, c, wait=0.8, show=True):
    s.write(c.encode() + b"\r"); s.flush(); time.sleep(wait)
    out = s.read(4000).decode("utf-8", "replace").replace("\r", "\n")
    if show:
        for ln in out.splitlines():
            if ln.strip(): print(f"   {ln.strip()}", flush=True)
    return out

def stats(s):
    out = cmd(s, "s", wait=1.0, show=False)
    for ln in out.splitlines():
        if ln.strip().startswith("ST "):
            return ln.strip()
    return ""

def f(line, key):
    m = re.search(rf"\b{key}=(-?\d+)", line)
    return int(m.group(1)) if m else None

buf, events = b"", []
with serial.Serial(dev, 115200, timeout=1) as s:
    s.reset_input_buffer()
    print("--- ringdown fix: raise the hot gate so desk ring is not 'still moving' ---", flush=True)
    cmd(s, "d1")    # hot gate thr>>2 (93 cts) -> thr>>1 (187 cts)
    cmd(s, "S")     # persist

    # True sample rate: samples delta over a measured idle interval.
    a = stats(s); t_a = time.monotonic()
    time.sleep(6)
    b = stats(s); t_b = time.monotonic()
    sa, sb = f(a, "dup"), f(b, "dup")   # dup rises with polling, not sampling
    print(f"\nIDLE  {b}", flush=True)

    print("\n### BUMP IT - 60s ###", flush=True)
    before = b
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
    el = time.monotonic() - t0
    after = stats(s)
    print(f"\nAFTER {after}", flush=True)

ev_b, ev_a = f(before, "ev"), f(after, "ev")
rj_b = sum(f(before, k) or 0 for k in ("rq", "ra", "rd"))
rj_a = sum(f(after,  k) or 0 for k in ("rq", "ra", "rd"))
acc, rej = (ev_a - ev_b), (rj_a - rj_b)
print(f"\naccepted {acc}   rejected {rej}   "
      f"({100*acc/(acc+rej):.0f}% accepted)" if acc + rej else "\nno candidates")
print(f"  rq {f(after,'rq')-f(before,'rq')}  "
      f"ra {f(after,'ra')-f(before,'ra')}  "
      f"rd {f(after,'rd')-f(before,'rd')}")
print(f"NDJSON captured: {len(events)}")
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    print(f"mg range {min(mgs)}-{max(mgs)}")
print(f"worst gap us: {f(before,'gap')} -> {f(after,'gap')}  (did the UI make it worse?)")
