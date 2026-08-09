"""Container format for arcade ROM sets pushed to the RA8D1's octo-SPI NOR.

The board's NOR is memory-mapped at 0x90000000, so the firmware parses this
header in place and points the emulator straight at the payload -- no copy,
no filesystem. Layout is deliberately flat and 4-byte aligned.

    off  size  field
      0     8  magic "RA8ARC01"
      8     4  machine id (1 = pacman)
     12     4  header size in bytes (64)
     16     4  cpu_off      offsets are from the start of the image
     20     4  cpu_len
     24     4  gfx_off
     28     4  gfx_len
     32     4  prom_off
     36     4  prom_len
     40     4  snd_off
     44     4  snd_len
     48     4  total_len    whole image including this header
     52     4  crc32        CRC-32/IEEE over [header_size, total_len)
     56     4  flags        bit 0: synthetic (not a real arcade ROM set)
     60     4  reserved

Everything is little-endian, matching the Cortex-M85.
"""

import struct
import zlib

MAGIC = b"RA8ARC01"
HDR_SIZE = 64
MACHINE_PACMAN = 1
FLAG_SYNTHETIC = 1

# Region sizes the firmware expects for the pacman machine.
PACMAN_ROM_SIZE = 0x4000
PACMAN_GFX_SIZE = 0x2000
PACMAN_PROM_SIZE = 0x0120
PACMAN_SND_SIZE = 0x0200


def _align4(n):
    return (n + 3) & ~3


def build(cpu, gfx, prom, snd=b"", machine=MACHINE_PACMAN, flags=0):
    """Assemble the regions into a complete image and return the bytes."""
    regions = []
    off = HDR_SIZE
    body = bytearray()

    for data in (cpu, gfx, prom, snd):
        data = bytes(data)
        regions.append((off if data else 0, len(data)))
        body += data
        pad = _align4(len(data)) - len(data)
        body += b"\x00" * pad
        off += len(data) + pad

    total = HDR_SIZE + len(body)
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF

    hdr = struct.pack(
        "<8sII" + "II" * 4 + "IIII",
        MAGIC,
        machine,
        HDR_SIZE,
        regions[0][0], regions[0][1],
        regions[1][0], regions[1][1],
        regions[2][0], regions[2][1],
        regions[3][0], regions[3][1],
        total,
        crc,
        flags,
        0,
    )
    assert len(hdr) == HDR_SIZE, len(hdr)
    return bytes(hdr) + bytes(body)


def describe(image):
    """Return a human-readable summary of an image, for the CLI tools."""
    (magic, machine, hdr_size,
     cpu_off, cpu_len, gfx_off, gfx_len,
     prom_off, prom_len, snd_off, snd_len,
     total, crc, flags, _) = struct.unpack("<8sII" + "II" * 4 + "IIII",
                                           image[:HDR_SIZE])
    calc = zlib.crc32(image[hdr_size:total]) & 0xFFFFFFFF
    lines = [
        f"magic     {magic.decode(errors='replace')}",
        f"machine   {machine}",
        f"cpu       off 0x{cpu_off:06x}  len {cpu_len} (0x{cpu_len:x})",
        f"gfx       off 0x{gfx_off:06x}  len {gfx_len} (0x{gfx_len:x})",
        f"prom      off 0x{prom_off:06x}  len {prom_len} (0x{prom_len:x})",
        f"sound     off 0x{snd_off:06x}  len {snd_len} (0x{snd_len:x})",
        f"total     {total} bytes",
        f"crc32     0x{crc:08x} ({'ok' if crc == calc else 'MISMATCH'})",
        f"flags     0x{flags:08x}"
        + ("  [synthetic]" if flags & FLAG_SYNTHETIC else ""),
    ]
    return "\n".join(lines)
