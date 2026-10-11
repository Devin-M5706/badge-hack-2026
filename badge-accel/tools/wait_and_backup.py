#!/usr/bin/env python3
"""Wait for the badge's port to become responsive, then identify it and back up
its flash. Read-only: nothing is written to the badge.

Watches for any Espressif-VID port and retries the open until it succeeds, so
the user can replug whenever — no timing coordination needed.
"""
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
BACKUP = r"C:\Users\dmyer\badge-accel\event-firmware-backup.bin"
DEADLINE_S = 15 * 60


def espressif_ports():
    return [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]


def try_open(dev):
    """A full open+configure, which is exactly what esptool needs to work."""
    try:
        serial.Serial(dev, 115200, timeout=0.5).close()
        return True
    except Exception:
        return False


def main():
    print("# watching for a responsive Espressif port...", flush=True)
    started = time.monotonic()
    port = None
    last_seen = None

    while time.monotonic() - started < DEADLINE_S:
        found = espressif_ports()
        if found != last_seen:
            print(f"# ports: {found or 'none'}", flush=True)
            last_seen = found
        for dev in found:
            if try_open(dev):
                port = dev
                break
        if port:
            break
        time.sleep(1)

    if not port:
        print("# TIMEOUT: port never became responsive", flush=True)
        return 1

    elapsed = time.monotonic() - started
    print(f"# PORT RESPONSIVE: {port} after {elapsed:.0f}s", flush=True)

    # Identify the chip. This also puts it into download mode via the
    # USB-Serial-JTAG reset sequence, so no strapping pin is needed.
    ident = subprocess.run(
        [sys.executable, "-m", "esptool", "--port", port,
         "--connect-attempts", "3", "chip-id"],
        capture_output=True, text=True, timeout=180)
    print(ident.stdout[-2000:], flush=True)
    if ident.returncode != 0:
        print(ident.stderr[-2000:], flush=True)
        print("# FAILED: could not identify chip", flush=True)
        return 1

    print("# backing up 4 MB of flash (read-only)...", flush=True)
    dump = subprocess.run(
        [sys.executable, "-m", "esptool", "--port", port,
         "--connect-attempts", "3", "read-flash", "0", "0x400000", BACKUP],
        capture_output=True, text=True, timeout=900)
    print(dump.stdout[-2000:], flush=True)
    if dump.returncode != 0:
        print(dump.stderr[-2000:], flush=True)
        print("# FAILED: backup incomplete", flush=True)
        return 1

    print(f"# BACKUP OK -> {BACKUP}", flush=True)
    print(f"# PORT={port}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
