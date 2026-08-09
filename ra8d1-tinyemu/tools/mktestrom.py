#!/usr/bin/env python3
"""Regenerate src/testrom.h from the built RV32 smoke payload.

    cd host/tests && make          # builds smoke.bin in the `br` container
    python3 tools/mktestrom.py

Kept as a script rather than a CMake custom command on purpose: building
smoke.bin needs a riscv toolchain that only exists inside Docker, and a Zephyr
build should not depend on Docker being up.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "host" / "tests" / "smoke.bin"
DST = ROOT / "src" / "testrom.h"

HEADER = """/*
 * testrom.h - the RV32 smoke-test payload, compiled in.
 *
 * GENERATED from host/tests/smoke.bin by tools/mktestrom.py. Do not edit.
 *
 * This is what the app boots when the image slot in flash is empty, so a
 * freshly flashed board proves the emulator, the SBI layer, the generated
 * devicetree and the Sv32 walk on real silicon without anyone having to push
 * a kernel first. Source: host/tests/smoke.c.
 */
#ifndef TESTROM_H_
#define TESTROM_H_

#include <stdint.h>

"""


def main() -> int:
    if not SRC.exists():
        print(f"{SRC} not found; run `make` in host/tests first", file=sys.stderr)
        return 1
    data = SRC.read_bytes()
    out = [HEADER, "static const uint8_t testrom[] = {\n"]
    for i in range(0, len(data), 12):
        row = " ".join(f"0x{b:02x}," for b in data[i:i + 12])
        out.append(f"\t{row}\n")
    out.append("};\n\n#endif /* TESTROM_H_ */\n")
    DST.write_text("".join(out))
    print(f"{DST}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
