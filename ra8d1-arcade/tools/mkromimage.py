#!/usr/bin/env python3
"""Package a Pac-Man ROM set you already own into an image for the board.

NO ROMS ARE SHIPPED WITH THIS PROJECT. Pac-Man's ROMs are copyrighted and
this tool will not fetch them. You supply the files; it only rearranges what
you point it at.

Accepts either a directory of loose files or a .zip laid out the way MAME's
"pacman" set is, with these members:

    pacman.6e  pacman.6f  pacman.6h  pacman.6j     4 KB each, program ROM
    pacman.5e  pacman.5f                           4 KB each, tiles/sprites
    82s123.7f                                      32 B,  palette PROM
    82s126.4a                                      256 B, colour lookup PROM
    82s126.1m  82s126.3m                           256 B each, sound PROMs

The "puckman" set is also accepted: same silicon, split into 2 KB parts.

    ./mkromimage.py ~/roms/pacman.zip pacman-image.bin

Each file is checked against the SHA-1 MAME records for it, so a bad dump is
reported rather than silently producing a machine that crashes in a confusing
way. Use --no-verify to skip that if you have a variant.
"""

import argparse
import hashlib
import os
import sys
import zipfile

import romimage

# SHA-1 digests as recorded in MAME's src/mame/pacman/pacman.cpp. These
# identify a good dump; they are not the ROM contents.
SHA1 = {
    "pacman.6e": "e87e059c5be45753f7e9f33dff851f16d6751181",
    "pacman.6f": "674d3a7f00d8be5e38b1fdc208ebef5a92d38329",
    "pacman.6h": "8e47e8c2c4d6117d174cdac150392042d3e0a881",
    "pacman.6j": "d4a70d56bb01d27d094d73db8667ffb00ca69cb9",
    "pacman.5e": "06ef227747a440831c9a3a613b76693d52a2f0a9",
    "pacman.5f": "4a937ac02216ea8c96477d4a15522070507fb599",
    "82s123.7f": "8d0268dee78e47c712202b0ec4f1f51109b1f2a5",
    "82s126.4a": "19097b5f60d1030f8b82d9f1d3a241f93e5c75d6",
    "82s126.1m": "bbcec0570aeceb582ff8238a4bc8546a23430081",
    "82s126.3m": "0c4d0bee858b97632411c440bea6948a74759746",
    # puckman: same regions, 2 KB parts.
    "pm1_prg1.6e": "813cecf44bf5464b1aed64b36f5047e4c79ba176",
    "pm1_prg2.6k": "b9ca52b63a49ddece768378d331deebbe34fe177",
    "pm1_prg3.6f": "9b5ddaaa8b564654f97af193dbcc29f81f230a25",
    "pm1_prg4.6m": "c2f00e1773c6864435f29c8b7f44f2ef85d227d3",
    "pm1_prg5.6h": "afe72fdfec66c145b53ed865f98734686b26e921",
    "pm1_prg6.6n": "08759833f7e0690b2ccae573c929e2a48e5bde7f",
    "pm1_prg7.6j": "d249fa9cdde774d5fee7258147cd25fa3f4dc2b3",
    "pm1_prg8.6p": "eb462de79f49b7aa8adb0cc6d31535b10550c0ce",
    "pm1_chg1.5e": "6d4ccc27d6be185589e08aa9f18702b679e49a4a",
    "pm1_chg2.5h": "79bb456be6c39c1ccd7d077fbe181523131fb300",
    "pm1_chg3.5f": "be933e691df4dbe7d12123913c3b7b7b585b7a35",
    "pm1_chg4.5j": "53771c573051db43e7185b1d188533056290a620",
    "pm1-1.7f": "8d0268dee78e47c712202b0ec4f1f51109b1f2a5",
    "pm1-4.4a": "19097b5f60d1030f8b82d9f1d3a241f93e5c75d6",
    "pm1-3.1m": "bbcec0570aeceb582ff8238a4bc8546a23430081",
    "pm1-2.3m": "0c4d0bee858b97632411c440bea6948a74759746",
}

# (region, [(member, offset, length), ...]) for each accepted set.
SETS = {
    "pacman": {
        "cpu": [("pacman.6e", 0x0000, 0x1000), ("pacman.6f", 0x1000, 0x1000),
                ("pacman.6h", 0x2000, 0x1000), ("pacman.6j", 0x3000, 0x1000)],
        "gfx": [("pacman.5e", 0x0000, 0x1000), ("pacman.5f", 0x1000, 0x1000)],
        "prom": [("82s123.7f", 0x0000, 0x0020), ("82s126.4a", 0x0020, 0x0100)],
        "snd": [("82s126.1m", 0x0000, 0x0100), ("82s126.3m", 0x0100, 0x0100)],
    },
    "puckman": {
        "cpu": [("pm1_prg1.6e", 0x0000, 0x800), ("pm1_prg2.6k", 0x0800, 0x800),
                ("pm1_prg3.6f", 0x1000, 0x800), ("pm1_prg4.6m", 0x1800, 0x800),
                ("pm1_prg5.6h", 0x2000, 0x800), ("pm1_prg6.6n", 0x2800, 0x800),
                ("pm1_prg7.6j", 0x3000, 0x800), ("pm1_prg8.6p", 0x3800, 0x800)],
        "gfx": [("pm1_chg1.5e", 0x0000, 0x800), ("pm1_chg2.5h", 0x0800, 0x800),
                ("pm1_chg3.5f", 0x1000, 0x800), ("pm1_chg4.5j", 0x1800, 0x800)],
        "prom": [("pm1-1.7f", 0x0000, 0x0020), ("pm1-4.4a", 0x0020, 0x0100)],
        "snd": [("pm1-3.1m", 0x0000, 0x0100), ("pm1-2.3m", 0x0100, 0x0100)],
    },
}

REGION_SIZE = {
    "cpu": romimage.PACMAN_ROM_SIZE,
    "gfx": romimage.PACMAN_GFX_SIZE,
    "prom": romimage.PACMAN_PROM_SIZE,
    "snd": romimage.PACMAN_SND_SIZE,
}


class Source:
    """Reads set members from either a zip or a directory, case-insensitively."""

    def __init__(self, path):
        self.path = path
        if zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path)
            names = self.zip.namelist()
        else:
            if not os.path.isdir(path):
                raise SystemExit(f"{path}: not a zip or a directory")
            self.zip = None
            names = os.listdir(path)
        self.index = {os.path.basename(n).lower(): n for n in names}

    def has(self, name):
        return name.lower() in self.index

    def read(self, name):
        real = self.index[name.lower()]
        if self.zip:
            return self.zip.read(real)
        with open(os.path.join(self.path, real), "rb") as f:
            return f.read()


def assemble(src, setname, verify):
    parts = SETS[setname]
    regions = {}
    problems = []

    for region, members in parts.items():
        buf = bytearray(REGION_SIZE[region])
        for member, off, length in members:
            if not src.has(member):
                problems.append(f"missing {member}")
                continue
            data = src.read(member)
            if len(data) != length:
                problems.append(
                    f"{member}: expected {length} bytes, got {len(data)}")
                continue
            if verify:
                got = hashlib.sha1(data).hexdigest()
                want = SHA1.get(member)
                if want and got != want:
                    problems.append(f"{member}: sha1 {got}, expected {want}")
            buf[off:off + length] = data
        regions[region] = bytes(buf)

    return regions, problems


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="pacman.zip, puckman.zip, or a directory")
    ap.add_argument("output", help="path to write the image to")
    ap.add_argument("--set", choices=sorted(SETS), default=None,
                    help="force a set instead of detecting it")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the SHA-1 check on each member")
    args = ap.parse_args()

    src = Source(args.source)

    setname = args.set
    if setname is None:
        for name, parts in SETS.items():
            first = parts["cpu"][0][0]
            if src.has(first):
                setname = name
                break
        if setname is None:
            raise SystemExit(
                f"{args.source}: does not look like any set I know "
                f"({', '.join(sorted(SETS))})")
    print(f"set: {setname}")

    regions, problems = assemble(src, setname, not args.no_verify)
    if problems:
        for p in problems:
            print(f"  error: {p}", file=sys.stderr)
        raise SystemExit("refusing to write an image from a bad set")

    img = romimage.build(regions["cpu"], regions["gfx"], regions["prom"],
                         regions["snd"])
    with open(args.output, "wb") as f:
        f.write(img)

    print(f"wrote {args.output}")
    print(romimage.describe(img))
    return 0


if __name__ == "__main__":
    sys.exit(main())
