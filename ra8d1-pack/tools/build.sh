#!/bin/zsh
# Build CircuitPython for the Renesas EK-RA8D1.
#
#   CP_ROOT     path to the circuitpython checkout   (default: repo containing this script)
#   ZEPHYR_SDK  path to the Zephyr SDK               (default: $HOME/zephyr-sdk-1.0.1)
#
# Requires the Zephyr build environment on PATH (west, cmake, ninja).
set -e

CP_ROOT="${CP_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
ZEPHYR_SDK="${ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
BOARD=renesas_ek_ra8d1

cd "$CP_ROOT/ports/zephyr-cp" || { echo "not a circuitpython checkout: $CP_ROOT"; exit 1; }

echo "=== BUILD START $(date) ==="
make BOARD=$BOARD 2>&1 | tail -40
echo "BUILD_RC=${pipestatus[1]}"

# Confirm the console mirror actually linked in, rather than trusting exit 0.
ELF="build-$BOARD/zephyr-cp/zephyr/zephyr.elf"
NM=$(command -v arm-zephyr-eabi-nm || echo "$ZEPHYR_SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm")
echo "=== console mirror symbol present? ==="
"$NM" "$ELF" 2>/dev/null | grep -ci uart_mirror_ready || echo 0
echo "=== DONE $(date) ==="
