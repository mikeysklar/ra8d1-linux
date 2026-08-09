#!/bin/zsh
# Flash CircuitPython to the Renesas EK-RA8D1 via the on-board J-Link (west jlink runner).
#
#   CP_ROOT  path to the circuitpython checkout (default: repo containing this script)
#
# Note: the J-Link's USB PID changes across a flash, so any pinned probe
# selector must be re-read from `probe-rs list` afterwards. "No connected
# probes were found" after a flash usually means a stale selector, not
# an unplugged board.
set -e

CP_ROOT="${CP_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
BOARD=renesas_ek_ra8d1

cd "$CP_ROOT/ports/zephyr-cp" || { echo "not a circuitpython checkout: $CP_ROOT"; exit 1; }

echo "=== probes before ==="
probe-rs list 2>&1 || true
echo "=== FLASH ==="
make BOARD=$BOARD flash 2>&1 | tail -20
echo "FLASH_RC=${pipestatus[1]}"
echo "=== probes after ==="
probe-rs list 2>&1 || true
echo "=== serial devices ==="
ls -1 /dev/cu.usbmodem* 2>&1 || true
