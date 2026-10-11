#!/usr/bin/env python3
"""Read the badge accelerometer CSV stream over USB and optionally log it.

    python read_accel.py                     # auto-detect port, print to stdout
    python read_accel.py --out accel.csv     # also append to a file
    python read_accel.py --port COM7         # pick the port yourself
    python read_accel.py --list              # show candidate ports

The badge enumerates as an Espressif USB-Serial-JTAG device (VID 0x303A).
Only one process may own the port, so close the Arduino Serial Monitor first.
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A


def candidates():
    """Ports that look like an ESP32 native-USB device, best guess first."""
    ports = list(list_ports.comports())
    espressif = [p for p in ports if p.vid == ESPRESSIF_VID]
    return espressif or ports


def pick_port():
    found = candidates()
    if not found:
        sys.exit("No serial ports found. Is the badge plugged in over USB-C?")
    if len(found) > 1:
        print(f"# {len(found)} candidate ports; using {found[0].device} "
              f"(override with --port)", file=sys.stderr)
    return found[0].device


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored on native USB, kept for compatibility")
    ap.add_argument("--out", help="append the stream to this CSV file")
    ap.add_argument("--list", action="store_true",
                    help="list candidate ports and exit")
    args = ap.parse_args()

    if args.list:
        for p in candidates():
            vid = f"{p.vid:04X}" if p.vid is not None else "----"
            print(f"{p.device}\tVID:{vid}\t{p.description}")
        return

    port = args.port or pick_port()
    print(f"# reading {port} (ctrl-c to stop)", file=sys.stderr)

    sink = open(args.out, "a", buffering=1) if args.out else None
    samples = 0
    started = time.monotonic()

    try:
        with serial.Serial(port, args.baud, timeout=1) as ser:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line)
                if sink:
                    sink.write(line + "\n")
                if not line.startswith(("#", "n,")):
                    samples += 1
    except serial.SerialException as e:
        sys.exit(f"Serial error: {e}")
    except KeyboardInterrupt:
        elapsed = time.monotonic() - started
        rate = samples / elapsed if elapsed > 0 else 0.0
        print(f"\n# {samples} samples in {elapsed:.1f}s ({rate:.1f} Hz)",
              file=sys.stderr)
        if args.out:
            print(f"# wrote {args.out}", file=sys.stderr)
    finally:
        if sink:
            sink.close()


if __name__ == "__main__":
    main()
