#!/bin/sh
# flash.sh — build and upload firmware to the badge, with the one piece of
# recovery advice you will actually need.
#
# Usage:
#   scripts/flash.sh                 # auto-detect port
#   scripts/flash.sh /dev/ttyACM0    # pick one
#   MONITOR=1 scripts/flash.sh       # open the serial monitor afterwards

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "Not inside a git repository." >&2
  exit 1
}
cd "$repo_root/firmware" || exit 1

# Prefer pio on PATH; fall back to the module, since pip --user installs often
# leave the console script off PATH.
if command -v pio >/dev/null 2>&1; then
  pio_cmd="pio"
else
  pio_cmd="python -m platformio"
fi

port="$1"
set -- run -e badge -t upload
[ -n "$port" ] && set -- "$@" --upload-port "$port"

echo "Flashing badge..."
# shellcheck disable=SC2086
$pio_cmd "$@"
upload_exit=$?

if [ "$upload_exit" -ne 0 ]; then
  cat >&2 <<'MSG'

─────────────────────────────────────────────────────────
 Upload failed. Before assuming the badge is bricked:
─────────────────────────────────────────────────────────

 If you saw "No serial data received", enter download mode:

   1. Unplug USB-C.
   2. HOLD the Start button down.
   3. Plug USB-C back in, still holding Start.
   4. Release, then re-run this script.

 A BLANK SCREEN in this state is expected — that is download
 mode, not a brick. Start (GPIO9) is a strapping pin, and this
 board has native USB with no auto-reset, so the manual hold is
 the normal procedure rather than a workaround. (HAL guide 1.4)

 Still nothing? Check that no serial monitor owns the port —
 only one process can hold it at a time. (HAL guide 1.5)

MSG
  exit "$upload_exit"
fi

echo
echo "Flashed."

if [ -n "$MONITOR" ]; then
  set -- device monitor -b 115200
  [ -n "$port" ] && set -- "$@" -p "$port"
  # shellcheck disable=SC2086
  exec $pio_cmd "$@"
fi

echo "Serial monitor:  cd firmware && pio device monitor -b 115200"
echo "Then type 'help' for the debug console."
