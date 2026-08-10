#!/bin/bash
# Fallback if static CPython does not fit or does not run: MicroPython's unix
# port, cross-built for riscv32 nommu / uClibc / bFLT. Runs INSIDE the container.
#
# This is not a substitute for CPython if the goal is Blinka -- it is a
# different interpreter with a different C API -- but it answers "can this
# board run *a* Python 3 at all", and it is ~1/20th the size, which matters a
# lot when every process needs one contiguous allocation.
#
# The `minimal` variant is used deliberately: it drops libffi (which we cannot
# have, no dlopen) and the readline/termios extras.
set -e

BR=/br/buildroot
HOSTBIN=$BR/output/host/bin
CROSS=riscv32-buildroot-linux-uclibc-
SRC=/br/micropython
STACK=${STACK:-262144}

export PATH=$HOSTBIN:$PATH
command -v ${CROSS}gcc >/dev/null || { echo "toolchain not built yet"; exit 1; }

# mpy-cross is a host tool and must be built with the host compiler
make -C "$SRC/mpy-cross" -j"$(nproc)" 2>&1 | tail -5

make -C "$SRC/ports/unix" VARIANT=minimal clean 2>&1 | tail -2 || true
make -C "$SRC/ports/unix" VARIANT=minimal -j"$(nproc)" \
	CROSS_COMPILE="$CROSS" \
	CFLAGS_EXTRA="-Os -DMICROPY_NLR_SETJMP=1 -DMICROPY_EMIT_RV32=0" \
	LDFLAGS_EXTRA="-Wl,-elf2flt=-s$STACK" \
	2>&1 | tail -25

BIN=$(find "$SRC/ports/unix" -name "micropython-minimal" -type f | head -1)
echo "=== result ==="
ls -l "$BIN"
file "$BIN"
${CROSS}flthdr "$BIN" 2>/dev/null || true
