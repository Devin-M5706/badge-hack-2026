import json, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]

def cmd(s, c, wait=0.8):
    s.write(c.encode() + b"\r"); s.flush(); time.sleep(wait)
    out = s.read(3000).decode("utf-8", "replace").replace("\r", "\n")
    for ln in out.splitlines():
        if ln.strip():
            print(f"   {ln.strip()}", flush=True)
    return out

buf, events = b"", []
with serial.Serial(dev, 115200, timeout=1) as s:
    s.reset_input_buffer()
    print("--- relaxing the two gates that rejected your bumps ---", flush=True)
    cmd(s, "n50")    # quiet credit 200ms -> 50ms  (isolation veto)
    cmd(s, "h20")    # max hot 10 -> 20            (ringdown veto)
    cmd(s, "z")      # zero counters + reseed, clean slate
    cmd(s, "s")
    print("\n### BUMP IT NOW - 60 second window ###", flush=True)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 60:
        c = s.read(256)
        if not c:
            continue
        buf += c
        while b"\r" in buf:
            ln, buf = buf.split(b"\r", 1)
            t = ln.decode("utf-8", "replace").strip()
            if t and t[0] == "{":
                events.append(t); print("   EVENT", t, flush=True)
    print("\n--- after ---", flush=True)
    cmd(s, "s")

print(f"\nNDJSON events: {len(events)}")
ok = sum(1 for e in events
         if (lambda d: isinstance(d.get("seq"), int) and isinstance(d.get("mg"), int))
            (json.loads(e)))
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    print(f"schema ok: {ok}/{len(events)}   mg range {min(mgs)}-{max(mgs)}")
    print("VERDICT:", "WIRE CONTRACT OK" if ok == len(events) else "SHAPE MISMATCH")
else:
    print("VERDICT: still none - next lever is w30 (shorter ringdown) or a0")
