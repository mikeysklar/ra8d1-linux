#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EK-RA8D1 rv32 Linux guest port
#
# SPDX-License-Identifier: MIT
"""Install the EK-RA8D1 rv32 Linux guest board into an installed Blinka.

This script edits an *explicitly named* site-packages directory. It will not
guess, and it will not touch the interpreter running it. Both --target and
--apply are required before anything is written; without --apply it prints
exactly what it would do and exits.

    python3 install.py --target /path/to/venv/lib/python3.11/site-packages
    python3 install.py --target /path/to/.../site-packages --apply

Two modes:

    --mode full   (default)  Add the chip and board to Adafruit_PlatformDetect
                             as well, so detection is automatic and the result
                             is what an upstream pull request would look like.

    --mode fast              Touch Blinka only. Detection is short-circuited by
                             BLINKA_FORCECHIP / BLINKA_FORCEBOARD instead. See
                             README.md, "The fastest path to a working demo".

Every edit is idempotent: running twice changes nothing the second time.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"

CHIP_ID = "RA8D1_PV"
BOARD_ID = "EK_RA8D1_RVLINUX"
CHIP_IMPORT = "adafruit_blinka.microcontroller.renesas.ra8d1_pv"
BOARD_IMPORT = "adafruit_blinka.board.renesas.ek_ra8d1_rvlinux"

# Files we add wholesale, relative to src/ and to the target site-packages.
NEW_FILES = [
    "adafruit_blinka/microcontroller/renesas/ra8d1_pv/__init__.py",
    "adafruit_blinka/microcontroller/renesas/ra8d1_pv/pin.py",
    "adafruit_blinka/board/renesas/__init__.py",
    "adafruit_blinka/board/renesas/ek_ra8d1_rvlinux.py",
]

# (relative path, anchor, insertion, placement)
# placement is "after" or "before". Every anchor was taken verbatim from the
# upstream files at the versions cited in README.md; if upstream moves, the
# script says which anchor it could not find rather than writing garbage.
PLATFORMDETECT_EDITS = [
    (
        "adafruit_platformdetect/constants/chips.py",
        'RZV2H = "RZV2H"\n',
        'RA8D1_PV = "RA8D1_PV"\n',
        "after",
    ),
    (
        "adafruit_platformdetect/constants/boards.py",
        "_KHADAS_40_PIN_IDS = (KHADAS_VIM3,)\n",
        'EK_RA8D1_RVLINUX = "EK_RA8D1_RVLINUX"\n\n',
        "before",
    ),
    (
        "adafruit_platformdetect/constants/boards.py",
        "_KHADAS_40_PIN_IDS = (KHADAS_VIM3,)\n",
        "\n_RENESAS_EK_IDS = (EK_RA8D1_RVLINUX,)\n",
        "after",
    ),
    (
        "adafruit_platformdetect/chip.py",
        '        if self.detector.check_dt_compatible_value("renesas,r9a09g056"):\n',
        """        # EK-RA8D1 riscv32 Linux guest, reaching real I2C/GPIO through the
        # paravirtual bridge at guest-physical 0x11200000.
        if self.detector.check_dt_compatible_value("renesas,ek-ra8d1-rvlinux"):
            return chips.RA8D1_PV
        ek_ra8d1_model = self.detector.get_device_model()
        if ek_ra8d1_model and "EK-RA8D1" in ek_ra8d1_model:
            return chips.RA8D1_PV
        # Last resort, and the only check that works on a guest booted with a
        # DTB that says nothing about the bridge: the bridge names its own i2c
        # adapter. The string is set in ra8d1-linux/guest/pv-io.c.
        for ek_ra8d1_bus in range(4):
            try:
                with open(
                    f"/sys/bus/i2c/devices/i2c-{ek_ra8d1_bus}/name",
                    "r",
                    encoding="utf-8",
                ) as ek_ra8d1_name:
                    if "EK-RA8D1 paravirt" in ek_ra8d1_name.read():
                        return chips.RA8D1_PV
            except OSError:
                pass

""",
        "before",
    ),
    (
        "adafruit_platformdetect/board.py",
        """        elif chip_id == chips.QCM6490:
            board_id = boards.PARTICLE_TACHYON
""",
        """        elif chip_id == chips.RA8D1_PV:
            board_id = boards.EK_RA8D1_RVLINUX
""",
        "after",
    ),
    (
        "adafruit_platformdetect/board.py",
        """    @property
    def any_embedded_linux(self) -> bool:
""",
        """    @property
    def any_renesas_ek_board(self) -> bool:
        \"\"\"Check whether the current board is any Renesas EK evaluation kit.\"\"\"
        return self.id in boards._RENESAS_EK_IDS

""",
        "before",
    ),
    (
        "adafruit_platformdetect/board.py",
        "            yield self.any_particle_board\n",
        "            yield self.any_renesas_ek_board\n",
        "after",
    ),
]


def fail(msg: str) -> "NoReturn":  # noqa: F821
    """Print an error and stop."""
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def copy_new_files(target: Path, apply: bool) -> None:
    """Copy the chip package and board module into the target Blinka."""
    for rel in NEW_FILES:
        src = SRC / rel
        dst = target / rel
        if dst.exists() and dst.read_bytes() == src.read_bytes():
            print(f"  ok (already current)  {rel}")
            continue
        verb = "overwrite" if dst.exists() else "create"
        print(f"  {verb:<21} {rel}")
        if apply:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)


def edit_json(path: Path, key: str, value: str, apply: bool) -> None:
    """Put key->value at the front of an import registry, idempotently."""
    if not path.exists():
        fail(f"{path} not found; is Blinka installed in this target?")
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get(key) == value and next(iter(data)) == key:
        print(f"  ok (already current)  {path.name}: {key}")
        return
    data.pop(key, None)
    merged = {key: value}
    merged.update(data)
    print(f"  set first entry        {path.name}: {key!r} -> {value!r}")
    if apply:
        path.write_text(json.dumps(merged, indent=4) + "\n", encoding="utf-8")


def edit_source(
    target: Path, rel: str, anchor: str, insertion: str, placement: str, apply: bool
) -> None:
    """Insert a block next to an anchor string, idempotently."""
    path = target / rel
    if not path.exists():
        fail(f"{path} not found; is Adafruit-PlatformDetect installed here?")
    text = path.read_text(encoding="utf-8")
    if insertion in text:
        print(f"  ok (already current)  {rel}")
        return
    if anchor not in text:
        fail(
            f"anchor not found in {rel}:\n---\n{anchor}---\n"
            "Upstream has moved. Apply this edit by hand and see README.md."
        )
    if text.count(anchor) != 1:
        fail(f"anchor appears {text.count(anchor)} times in {rel}; refusing to guess")
    new = (
        text.replace(anchor, anchor + insertion)
        if placement == "after"
        else text.replace(anchor, insertion + anchor)
    )
    print(f"  insert {placement:<14} {rel}")
    if apply:
        path.write_text(new, encoding="utf-8")


def main() -> None:
    """Entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--target",
        required=True,
        type=Path,
        help="site-packages directory to modify (required, never guessed)",
    )
    parser.add_argument(
        "--mode", choices=("full", "fast"), default="full", help="see module docstring"
    )
    parser.add_argument(
        "--apply", action="store_true", help="actually write; otherwise dry run"
    )
    args = parser.parse_args()

    target: Path = args.target.resolve()
    if not target.is_dir():
        fail(f"{target} is not a directory")
    if not (target / "adafruit_blinka").is_dir():
        fail(f"{target} does not contain adafruit_blinka")

    print(f"target: {target}")
    print(f"mode:   {args.mode}")
    print("" if args.apply else "DRY RUN, nothing will be written. Pass --apply.\n")

    print("Blinka files:")
    copy_new_files(target, args.apply)

    print("Blinka import registries:")
    edit_json(target / "microcontroller_imports.json", CHIP_ID, CHIP_IMPORT, args.apply)
    if args.mode == "full":
        edit_json(target / "board_imports.json", BOARD_ID, BOARD_IMPORT, args.apply)
    else:
        # board_imports.json keys are resolved with getattr(ap_board, key) in
        # Adafruit_Blinka src/board.py line 42, so a key that is not a real
        # constant in adafruit_platformdetect.constants.boards raises
        # AttributeError and breaks `import board` for every board. In fast
        # mode we have not added the constant, so we borrow one that exists.
        edit_json(
            target / "board_imports.json", "GENERIC_LINUX_PC", BOARD_IMPORT, args.apply
        )

    if args.mode == "full":
        print("PlatformDetect:")
        for rel, anchor, insertion, placement in PLATFORMDETECT_EDITS:
            edit_source(target, rel, anchor, insertion, placement, args.apply)
    else:
        print("PlatformDetect: untouched (fast mode)")
        print("  export BLINKA_FORCECHIP=RA8D1_PV")
        print("  export BLINKA_FORCEBOARD=GENERIC_LINUX_PC")

    print("\ndone." if args.apply else "\ndry run complete.")


if __name__ == "__main__":
    main()
