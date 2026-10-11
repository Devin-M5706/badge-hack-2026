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
    out = cmd(s, "s", show=False)
    for ln in out.splitlines():
        if ln.strip().startswith("ST "):
            return ln.strip()
    return ""

def field(line, key):
    m = re.search(rf"\b{key}=(-?\d+)", line)
    return int(m.group(1)) if m else None

buf, events = b"", []
with serial.Serial(dev, 115200, timeout=1) as s:
    s.reset_input_buffer()
    print("--- tuning (and SAVING to NVS this time) ---", flush=True)
    cmd(s, "p1")     # blip penalty 8 -> 1
    cmd(s, "n20")    # quiet credit -> 20 ms
    cmd(s, "h20")    # max hot 10 -> 20
    cmd(s, "a0")     # agitation veto off
    cmd(s, "S")      # persist to NVS so a reboot keeps it
    cmd(s, "z")
    time.sleep(1)
    before = stats(s); print(f"BEFORE {before}", flush=True)
    t0 = time.monotonic()
    print("\n### BUMP IT - 60s - watch the screen and LEDs ###", flush=True)
    while time.monotonic() - t0 < 60:
        c = s.read(256)
        if not c: continue
        buf += c
        while b"\r" in buf:
            ln, buf = buf.split(b"\r", 1)
            t = ln.decode("utf-8", "replace").strip()
            if t and t[0] == "{":
                events.append(t); print("   EVENT", t, flush=True)
    elapsed = time.monotonic() - t0
    after = stats(s); print(f"\nAFTER  {after}", flush=True)

print(f"\nevents: {len(events)}")
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    print(f"mg range {min(mgs)}-{max(mgs)}")
gb, ga = field(before, "gap"), field(after, "gap")
hb, ha = field(before, "hz"),  field(after, "hz")
if None not in (gb, ga):
    print(f"sampler gaps during window: {ga-gb} over {elapsed:.0f}s "
          f"({(ga-gb)/elapsed:.1f}/s)")
print(f"hz before={hb} after={ha}  (pre-UI baseline was 515)")
print("STARVATION:", "none" if (ga-gb) < 50 else "UI IS COSTING SAMPLES")
