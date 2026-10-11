import json, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]

def stats(s, label):
    s.reset_input_buffer(); s.write(b"s\r"); s.flush(); time.sleep(1.0)
    for ln in s.read(2000).decode("utf-8", "replace").replace("\r", "\n").splitlines():
        if ln.startswith("ST "):
            print(f"{label}: {ln.strip()}", flush=True)
            return ln
    return ""

buf, events, chatter = b"", [], []
with serial.Serial(dev, 115200, timeout=1) as s:
    before = stats(s, "BEFORE")
    print("### BUMP THE BADGE NOW - 90 second window ###", flush=True)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 90:
        c = s.read(256)
        if not c:
            continue
        buf += c
        while b"\r" in buf:
            ln, buf = buf.split(b"\r", 1)
            t = ln.decode("utf-8", "replace").strip()
            if not t:
                continue
            if t[0] == "{":
                events.append(t); print("   EVENT", t, flush=True)
            else:
                chatter.append(t)
    after = stats(s, "AFTER ")

print(f"\nNDJSON events: {len(events)}  other lines: {len(chatter)}")
ok = 0
for e in events:
    try:
        d = json.loads(e)
        if isinstance(d.get("seq"), int) and isinstance(d.get("mg"), int):
            ok += 1
    except Exception:
        pass
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    print(f"parse clean with numeric seq+mg: {ok}/{len(events)}")
    print(f"mg range {min(mgs)}-{max(mgs)}")
    print("VERDICT:", "WIRE CONTRACT OK" if ok == len(events) else "SHAPE MISMATCH")
else:
    print("VERDICT: still no events - compare ev= and pk= above")
