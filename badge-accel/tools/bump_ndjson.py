import json, time, serial
from serial.tools import list_ports
dev = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]
buf, events, chatter = b"", [], []
with serial.Serial(dev, 115200, timeout=1) as s:
    t0 = time.monotonic()
    while time.monotonic() - t0 < 25:
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
                events.append(t)
            else:
                chatter.append(t)
print(f"NDJSON events: {len(events)}   other lines: {len(chatter)}")
for e in events[:10]:
    print("   ", e)
ok = 0
for e in events:
    try:
        d = json.loads(e)
        if isinstance(d.get("seq"), int) and isinstance(d.get("mg"), int):
            ok += 1
    except Exception:
        pass
print(f"parse clean with numeric seq+mg: {ok}/{len(events)}")
if events:
    mgs = [json.loads(e)["mg"] for e in events]
    seqs = [json.loads(e)["seq"] for e in events]
    print(f"mg range {min(mgs)}-{max(mgs)}, seq {min(seqs)}..{max(seqs)}")
    print("VERDICT:", "WIRE CONTRACT OK" if ok == len(events) else "SHAPE MISMATCH")
else:
    print("VERDICT: no bumps captured")
