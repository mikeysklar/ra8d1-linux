#!/bin/bash
# Build an offline wheelhouse of CircuitPython driver libraries for the guest.
#
# Runs on the DEV MACHINE (or in the `br` container), never on the board. The
# guest has no network, so `pip install` from PyPI cannot work there; this
# stages the wheels so the guest can install from a local directory instead:
#
#     pip install --user --no-index --find-links=/opt/wheels adafruit-circuitpython-bme280
#
# Copy the result into the rootfs at /opt/wheels before building the image.
#
# WHY THIS IS SAFE TO DO ON AN x86/arm64 HOST: everything collected here must
# be a pure-Python wheel, tagged py3-none-any, which has no architecture in it
# at all. That is checked below and the script fails if anything else appears.
# Anything with a C extension would need a riscv32 wheel, and essentially none
# exist - the guest also has no compiler, so it cannot build one. That is the
# same wall adafruit-blinka itself hits via its sysv_ipc dependency, which is
# why Blinka is installed into the image rather than pip-installed.
#
# A CLEAN RUN HERE DOES NOT MEAN INSTALLS WILL RESOLVE ON THE GUEST. pip
# evaluates environment markers against the machine it runs on. adafruit-blinka
# needs sysv_ipc only under `sys_platform == "linux"`, which is FALSE on a Mac,
# so on darwin pip never asks for it and all 21 wheels pass the purity gate
# below. On the riscv32 guest the marker is true and the install dies with
# "No matching distribution found for sysv_ipc". That is not a defect in this
# wheelhouse - it is why usage.md tells you to install with --no-deps and name
# the CircuitPython dependencies yourself.
set -euo pipefail

OUT=${1:-./wheelhouse}
shift || true

# Common drivers. Override by passing package names as arguments.
PKGS=("$@")
if [ ${#PKGS[@]} -eq 0 ]; then
	PKGS=(
		adafruit-circuitpython-busdevice
		adafruit-circuitpython-register
		adafruit-circuitpython-bme280
		adafruit-circuitpython-bmp280
		adafruit-circuitpython-ssd1306
		adafruit-circuitpython-mcp230xx
		adafruit-circuitpython-ads1x15
		adafruit-circuitpython-neopixel
	)
fi

mkdir -p "$OUT"

# --no-deps would miss adafruit-circuitpython-busdevice and friends; let pip
# resolve, then verify purity afterwards rather than trusting the resolution.
python3 -m pip download \
	--only-binary=:all: \
	--python-version 3.11 \
	--implementation cp \
	--abi none --platform any \
	-d "$OUT" \
	"${PKGS[@]}"

echo
echo "=== verifying every wheel is pure-Python (py3-none-any) ==="
bad=0
for w in "$OUT"/*.whl; do
	case "$(basename "$w")" in
	*-py3-none-any.whl | *-py2.py3-none-any.whl) ;;
	*)
		echo "NOT PORTABLE: $(basename "$w")"
		bad=1
		;;
	esac
done
if [ "$bad" -ne 0 ]; then
	echo
	echo "A non-pure wheel cannot run on the riscv32 guest. Remove that package"
	echo "or find a pure-Python equivalent; do NOT ship it and hope."
	exit 1
fi

echo "all $(ls -1 "$OUT"/*.whl | wc -l | tr -d ' ') wheels are pure-Python"
echo
echo "next: copy $OUT into the rootfs as /opt/wheels, then on the guest:"
echo "  pip install --user --no-index --find-links=/opt/wheels <package>"
