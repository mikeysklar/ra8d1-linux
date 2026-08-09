#!/usr/bin/env python3
"""Generate a synthetic Pac-Man-hardware ROM image for testing.

Nothing here is derived from a real arcade ROM. The program ROM, the tile and
sprite ROMs and both PROMs are all generated from scratch, so this image is
free to commit and to hand around. Its job is to exercise the whole path --
container parsing, gfx decode, the awkward video RAM mapping, the palette
ladder, sprites, mode-2 interrupts and the watchdog -- without needing
anything copyrighted.

    ./mktestrom.py testrom.bin

If the picture that comes out of host/test_pacman is upright and readable,
the orientation and bit-order work is right; a mirrored or transposed font is
immediately obvious, which is the whole reason the test pattern is text.
"""

import argparse
import sys

import romimage

# ------------------------------------------------------------------ font

# 8x8 glyphs in the orientation we want to SEE on the portrait screen.
# '.' is colour 0 (transparent/black), '#' is colour 1, '+' is colour 2.
FONT = {
    ' ': ("........", "........", "........", "........",
          "........", "........", "........", "........"),
    'A': ("..###...", ".#...#..", "#.....#.", "#.....#.",
          "#######.", "#.....#.", "#.....#.", "........"),
    'B': ("######..", "#.....#.", "#.....#.", "######..",
          "#.....#.", "#.....#.", "######..", "........"),
    'C': ("..####..", ".#....#.", "#.......", "#.......",
          "#.......", ".#....#.", "..####..", "........"),
    'D': ("#####...", "#....#..", "#.....#.", "#.....#.",
          "#.....#.", "#....#..", "#####...", "........"),
    'E': ("#######.", "#.......", "#.......", "#####...",
          "#.......", "#.......", "#######.", "........"),
    'F': ("#######.", "#.......", "#.......", "#####...",
          "#.......", "#.......", "#.......", "........"),
    'G': ("..####..", ".#....#.", "#.......", "#..####.",
          "#.....#.", ".#....#.", "..####..", "........"),
    'H': ("#.....#.", "#.....#.", "#.....#.", "#######.",
          "#.....#.", "#.....#.", "#.....#.", "........"),
    'I': (".#####..", "...#....", "...#....", "...#....",
          "...#....", "...#....", ".#####..", "........"),
    'J': ("....###.", ".....#..", ".....#..", ".....#..",
          "#....#..", "#....#..", ".####...", "........"),
    'K': ("#....#..", "#...#...", "#..#....", "###.....",
          "#..#....", "#...#...", "#....#..", "........"),
    'L': ("#.......", "#.......", "#.......", "#.......",
          "#.......", "#.......", "#######.", "........"),
    'M': ("#.....#.", "##...##.", "#.#.#.#.", "#..#..#.",
          "#.....#.", "#.....#.", "#.....#.", "........"),
    'N': ("#.....#.", "##....#.", "#.#...#.", "#..#..#.",
          "#...#.#.", "#....##.", "#.....#.", "........"),
    'O': ("..###...", ".#...#..", "#.....#.", "#.....#.",
          "#.....#.", ".#...#..", "..###...", "........"),
    'P': ("######..", "#.....#.", "#.....#.", "######..",
          "#.......", "#.......", "#.......", "........"),
    'Q': ("..###...", ".#...#..", "#.....#.", "#.....#.",
          "#...#.#.", ".#...#..", "..###.#.", "........"),
    'R': ("######..", "#.....#.", "#.....#.", "######..",
          "#...#...", "#....#..", "#.....#.", "........"),
    'S': ("..####..", ".#....#.", "#.......", "..###...",
          "......#.", "#.....#.", ".#####..", "........"),
    'T': ("#######.", "...#....", "...#....", "...#....",
          "...#....", "...#....", "...#....", "........"),
    'U': ("#.....#.", "#.....#.", "#.....#.", "#.....#.",
          "#.....#.", "#.....#.", ".#####..", "........"),
    'V': ("#.....#.", "#.....#.", "#.....#.", ".#...#..",
          ".#...#..", "..#.#...", "...#....", "........"),
    'W': ("#.....#.", "#.....#.", "#.....#.", "#..#..#.",
          "#.#.#.#.", "##...##.", "#.....#.", "........"),
    'X': ("#.....#.", ".#...#..", "..#.#...", "...#....",
          "..#.#...", ".#...#..", "#.....#.", "........"),
    'Y': ("#.....#.", ".#...#..", "..#.#...", "...#....",
          "...#....", "...#....", "...#....", "........"),
    'Z': ("#######.", ".....#..", "....#...", "...#....",
          "..#.....", ".#......", "#######.", "........"),
    '0': ("..###...", ".#...#..", "#....##.", "#..#..#.", "##...#..",
          ".#...#..", "..###...", "........"),
    '1': ("...#....", "..##....", ".#.#....", "...#....",
          "...#....", "...#....", ".#####..", "........"),
    '2': ("..###...", ".#...#..", ".....#..", "....#...",
          "..#.....", ".#......", "#######.", "........"),
    '3': ("..###...", ".#...#..", ".....#..", "...##...",
          ".....#..", ".#...#..", "..###...", "........"),
    '4': ("....##..", "...#.#..", "..#..#..", ".#...#..",
          "#######.", ".....#..", ".....#..", "........"),
    '5': ("#######.", "#.......", "######..", "......#.",
          "......#.", "#.....#.", ".#####..", "........"),
    '6': ("..####..", ".#......", "#.......", "######..",
          "#.....#.", "#.....#.", ".#####..", "........"),
    '7': ("#######.", "......#.", ".....#..", "....#...",
          "...#....", "...#....", "...#....", "........"),
    '8': ("..###...", ".#...#..", ".#...#..", "..###...",
          ".#...#..", ".#...#..", "..###...", "........"),
    '9': ("..####..", ".#....#.", "#.....#.", "..#####.",
          "......#.", ".....#..", "..###...", "........"),
    '-': ("........", "........", "........", "######..",
          "........", "........", "........", "........"),
    '.': ("........", "........", "........", "........",
          "........", "...##...", "...##...", "........"),
    ':': ("........", "...##...", "...##...", "........",
          "...##...", "...##...", "........", "........"),
    '*': ("........", "#..#..#.", ".#.#.#..", "..###...",
          ".#.#.#..", "#..#..#.", "........", "........"),
    '/': ("......#.", ".....#..", "....#...", "...#....",
          "..#.....", ".#......", "#.......", "........"),
    '=': ("........", "........", "#######.", "........",
          "#######.", "........", "........", "........"),
}

PIXCHR = {'.': 0, '#': 1, '+': 2, '@': 3}


# ----------------------------------------------------------- gfx encoding

def encode_tile(px):
    """px[fy][fx] -> 16 bytes in Pac-Man tile ROM format.

    Inverse of the decoder in emu/pacman.c. The stored orientation is
    landscape, so the portrait pixel we want on screen at (fx, fy) lives at
    landscape (lx, ly) = (fy, 7 - fx).
    """
    out = bytearray(16)
    for lx in range(8):
        for ly in range(8):
            v = px[lx][7 - ly]          # portrait fy = lx, fx = 7 - ly
            idx = (8 if lx < 4 else 0) + ly
            k = lx & 3
            if v & 1:
                out[idx] |= 1 << (7 - k)
            if v & 2:
                out[idx] |= 1 << (3 - k)
    return bytes(out)


def encode_sprite(px):
    """px[fy][fx] 16x16 -> 64 bytes in Pac-Man sprite ROM format."""
    xbase = (8, 16, 24, 0)
    out = bytearray(64)
    for lx in range(16):
        for ly in range(16):
            v = px[lx][15 - ly]         # portrait fy = lx, fx = 15 - ly
            ybyte = ly if ly < 8 else 32 + ly - 8
            idx = xbase[lx >> 2] + ybyte
            k = lx & 3
            if v & 1:
                out[idx] |= 1 << (7 - k)
            if v & 2:
                out[idx] |= 1 << (3 - k)
    return bytes(out)


def glyph_px(rows):
    return [[PIXCHR[c] for c in row] for row in rows]


def build_gfx():
    """4K of tiles followed by 4K of sprites."""
    tiles = [[[0] * 8 for _ in range(8)] for _ in range(256)]

    def solid(n, v):
        tiles[n] = [[v] * 8 for _ in range(8)]

    solid(0x00, 0)
    solid(0x01, 1)
    solid(0x02, 2)
    solid(0x03, 3)

    # 0x04 box outline, 0x05 checkerboard, 0x06 corner L, 0x07/0x08 stripes,
    # 0x09 diagonal. These give unambiguous orientation cues.
    tiles[0x04] = [[1 if (x in (0, 7) or y in (0, 7)) else 0
                    for x in range(8)] for y in range(8)]
    tiles[0x05] = [[1 if ((x ^ y) & 1) else 2 for x in range(8)]
                   for y in range(8)]
    tiles[0x06] = [[1 if (x == 0 or y == 0) else 0 for x in range(8)]
                   for y in range(8)]
    tiles[0x07] = [[1 if (y & 1) else 0 for x in range(8)] for y in range(8)]
    tiles[0x08] = [[1 if (x & 1) else 0 for x in range(8)] for y in range(8)]
    tiles[0x09] = [[1 if x == y else 0 for x in range(8)] for y in range(8)]

    # ASCII font at its own code point, so tile code == character.
    for ch, rows in FONT.items():
        tiles[ord(ch)] = glyph_px(rows)

    # Fill the unused upper half with a per-tile pattern so the ROM is not
    # blank and a stray tile code is visible rather than silent.
    for n in range(0x60, 0x100):
        tiles[n] = [[((n + x + y) & 3) for x in range(8)] for y in range(8)]

    tile_rom = b"".join(encode_tile(t) for t in tiles)
    assert len(tile_rom) == 0x1000, len(tile_rom)

    sprites = [[[0] * 16 for _ in range(16)] for _ in range(64)]

    # 0: filled disc. 1: asymmetric arrow -- a mirrored sprite is obvious.
    # 2: ring. 3: solid square with one corner notched.
    for y in range(16):
        for x in range(16):
            dx, dy = x - 7.5, y - 7.5
            r2 = dx * dx + dy * dy
            if r2 <= 49:
                sprites[0][y][x] = 1
            if 25 <= r2 <= 49:
                sprites[2][y][x] = 2
    for y in range(16):
        for x in range(16):
            # Arrow pointing up (towards lower y) with a tail on the left.
            if abs(x - 8) <= y // 2 and y < 10:
                sprites[1][y][x] = 1
            elif 10 <= y < 16 and 6 <= x <= 8:
                sprites[1][y][x] = 2
    for y in range(16):
        for x in range(16):
            if x + y > 6:
                sprites[3][y][x] = 3

    for n in range(4, 64):
        bar = n & 15
        for y in range(16):
            for x in range(16):
                if y < 2 or x < 2:
                    sprites[n][y][x] = 1
                elif y >= 4 and x < 2 + bar:
                    sprites[n][y][x] = 2

    sprite_rom = b"".join(encode_sprite(s) for s in sprites)
    assert len(sprite_rom) == 0x1000, len(sprite_rom)

    return tile_rom + sprite_rom


def build_proms():
    """32-byte palette PROM followed by the 256-byte colour lookup PROM."""
    # Palette byte: bits 0-2 red (1k, 470, 220), bits 3-5 green, bits 6-7
    # blue (470, 220). Entry 0 must be black -- sprites treat colour table
    # entry 0 as transparent.
    pal = [0x00] * 32
    pal[1] = 0xFF   # white
    pal[2] = 0x07   # red
    pal[3] = 0x38   # green
    pal[4] = 0xC0   # blue
    pal[5] = 0x3F   # yellow
    pal[6] = 0xF8   # cyan
    pal[7] = 0xC7   # magenta
    pal[8] = 0x17   # orange
    pal[9] = 0x24   # dim green
    pal[10] = 0x80  # dim blue
    pal[11] = 0x03  # dim red
    pal[12] = 0x1B  # olive
    pal[13] = 0xDB  # pale violet
    pal[14] = 0x6D  # pale cyan
    pal[15] = 0x92  # slate
    for i in range(16, 32):
        pal[i] = pal[i - 16]

    ctab = bytearray(256)
    for p in range(64):
        ctab[p * 4 + 0] = 0
        ctab[p * 4 + 1] = (p % 15) + 1
        ctab[p * 4 + 2] = ((p + 5) % 15) + 1
        ctab[p * 4 + 3] = ((p + 10) % 15) + 1

    return bytes(pal) + bytes(ctab)


# --------------------------------------------------------- video RAM map

TILE_COLS, TILE_ROWS = 28, 36


def vram_offset(tx, ty):
    """Portrait tile (col, row) -> video RAM offset.

    Independent restatement of the board's address decode; emu/pacman.c
    builds the same table. Landscape col = portrait row, landscape row =
    27 - portrait col.
    """
    c = ty - 2
    r = (27 - tx) + 2
    if c & 0x20:
        return (r + ((c & 0x1F) << 5)) & 0x3FF
    return (c + (r << 5)) & 0x3FF


def build_screen():
    """Draw the test pattern and return (vram, cram), 1K each."""
    tile = [[0x20] * TILE_COLS for _ in range(TILE_ROWS)]   # spaces
    colr = [[1] * TILE_COLS for _ in range(TILE_ROWS)]

    def text(tx, ty, s, c=1):
        for i, ch in enumerate(s.upper()):
            if 0 <= tx + i < TILE_COLS and 0 <= ty < TILE_ROWS:
                tile[ty][tx + i] = ord(ch) if ch in FONT else ord(' ')
                colr[ty][tx + i] = c

    # Score rows: the top two rows live at the far end of video RAM, so
    # getting these right proves the odd part of the mapping.
    text(1, 0, "RA8D1", 2)
    text(9, 0, "ARCADE", 3)
    text(21, 0, "M85", 4)
    text(1, 1, "PACMAN HARDWARE TEST", 5)

    # Playfield border, rows 3..32.
    for ty in range(3, 33):
        for tx in range(TILE_COLS):
            edge = (ty in (3, 32)) or (tx in (0, TILE_COLS - 1))
            if edge:
                tile[ty][tx] = 0x04
                colr[ty][tx] = 6

    text(2, 5, "PORTRAIT 224X288", 1)
    text(2, 7, "TILES 28X36", 3)
    text(2, 9, "IRQ MODE 2", 5)
    text(2, 11, "FRAME:", 1)

    # Corner markers inside the border: reading TL/TR/BL/BR in the right
    # places rules out a transpose or a mirror.
    text(1, 4, "TL", 2)
    text(25, 4, "TR", 2)
    text(1, 31, "BL", 4)
    text(25, 31, "BR", 4)

    # A diagonal of solid tiles: a transposed map bends it the wrong way.
    for k in range(20):
        tx, ty = 4 + k, 12 + k
        if tx < TILE_COLS - 1 and ty < 32:
            tile[ty][tx] = 0x01
            colr[ty][tx] = (k % 30) + 1

    # Palette sweep strip, one column per colour attribute.
    for k in range(24):
        tile[30][2 + k] = 0x02
        colr[30][2 + k] = k + 1

    # Bottom two rows: the other half of the odd mapping.
    text(1, 34, "CREDIT 0", 5)
    text(14, 34, "SYNTHETIC", 2)
    text(1, 35, "NO ARCADE ROM USED", 3)

    vram = bytearray(0x400)
    cram = bytearray(0x400)
    for ty in range(TILE_ROWS):
        for tx in range(TILE_COLS):
            off = vram_offset(tx, ty)
            vram[off] = tile[ty][tx]
            cram[off] = colr[ty][tx]
    return bytes(vram), bytes(cram)


# ------------------------------------------------------- tiny Z80 assembler

class Asm:
    """Just enough Z80 to write the test program. Two-pass label fixups."""

    def __init__(self, org=0):
        self.org = org
        self.buf = bytearray()
        self.labels = {}
        self.fix16 = []     # (offset_in_buf, label)
        self.fix8 = []      # (offset_in_buf, label) relative

    # -- assembler plumbing
    @property
    def pc(self):
        return self.org + len(self.buf)

    def label(self, name):
        self.labels[name] = self.pc
        return self

    def db(self, *vals):
        for v in vals:
            self.buf.append(v & 0xFF)
        return self

    def dw_label(self, name):
        self.fix16.append((len(self.buf), name))
        self.buf += b"\x00\x00"
        return self

    def _nn(self, v):
        if isinstance(v, str):
            self.fix16.append((len(self.buf), v))
            self.buf += b"\x00\x00"
        else:
            self.buf.append(v & 0xFF)
            self.buf.append((v >> 8) & 0xFF)

    def _e(self, label):
        self.fix8.append((len(self.buf), label))
        self.buf.append(0)

    def at(self, addr):
        """Pad with 0xFF up to an absolute address."""
        while self.pc < addr:
            self.buf.append(0xFF)
        assert self.pc == addr, f"overran {addr:#06x} (at {self.pc:#06x})"
        return self

    def link(self, size):
        out = bytearray(self.buf)
        for off, name in self.fix16:
            a = self.labels[name]
            out[off] = a & 0xFF
            out[off + 1] = (a >> 8) & 0xFF
        for off, name in self.fix8:
            delta = self.labels[name] - (self.org + off + 1)
            assert -128 <= delta <= 127, f"jr out of range to {name}"
            out[off] = delta & 0xFF
        out += b"\xFF" * (size - len(out))
        assert len(out) == size, f"program is {len(out)} bytes, max {size}"
        return bytes(out)

    # -- instructions
    def di(self):       return self.db(0xF3)
    def ei(self):       return self.db(0xFB)
    def im2(self):      return self.db(0xED, 0x5E)
    def ldir(self):     return self.db(0xED, 0xB0)
    def reti(self):     return self.db(0xED, 0x4D)
    def ret(self):      return self.db(0xC9)
    def halt(self):     return self.db(0x76)
    def xor_a(self):    return self.db(0xAF)
    def inc_a(self):    return self.db(0x3C)
    def inc_hl(self):   return self.db(0x23)
    def dec_bc(self):   return self.db(0x0B)
    def ld_a_b(self):   return self.db(0x78)
    def or_c(self):     return self.db(0xB1)
    def ld_i_a(self):   return self.db(0xED, 0x47)
    def ld_hl_ind(self):    return self.db(0x7E)   # ld a,(hl)
    def ld_ind_hl_a(self):  return self.db(0x77)   # ld (hl),a
    def ex_de_hl(self):     return self.db(0xEB)
    def add_hl_de(self):    return self.db(0x19)
    def push_af(self):  return self.db(0xF5)
    def pop_af(self):   return self.db(0xF1)
    def push_hl(self):  return self.db(0xE5)
    def pop_hl(self):   return self.db(0xE1)
    def push_bc(self):  return self.db(0xC5)
    def pop_bc(self):   return self.db(0xC1)
    def push_de(self):  return self.db(0xD5)
    def pop_de(self):   return self.db(0xD1)

    def ld_a(self, n):  return self.db(0x3E, n)
    def ld_b(self, n):  return self.db(0x06, n)
    def ld_c(self, n):  return self.db(0x0E, n)
    def and_n(self, n): return self.db(0xE6, n)
    def add_n(self, n): return self.db(0xC6, n)
    def cp_n(self, n):  return self.db(0xFE, n)
    def out_n(self, n): return self.db(0xD3, n)

    def ld_hl(self, nn):    self.db(0x21); self._nn(nn); return self
    def ld_de(self, nn):    self.db(0x11); self._nn(nn); return self
    def ld_bc(self, nn):    self.db(0x01); self._nn(nn); return self
    def ld_sp(self, nn):    self.db(0x31); self._nn(nn); return self
    def ld_mem_a(self, nn): self.db(0x32); self._nn(nn); return self
    def ld_a_mem(self, nn): self.db(0x3A); self._nn(nn); return self
    def ld_mem_hl(self, nn): self.db(0x22); self._nn(nn); return self
    def ld_hl_mem(self, nn): self.db(0x2A); self._nn(nn); return self
    def ld_ind_hl_n(self, n): return self.db(0x36, n)

    def jp(self, nn):       self.db(0xC3); self._nn(nn); return self
    def call(self, nn):     self.db(0xCD); self._nn(nn); return self
    def jr(self, lbl):      self.db(0x18); self._e(lbl); return self
    def jr_nz(self, lbl):   self.db(0x20); self._e(lbl); return self
    def jr_z(self, lbl):    self.db(0x28); self._e(lbl); return self
    def djnz(self, lbl):    self.db(0x10); self._e(lbl); return self


# Work RAM scratch. 0x4C00-0x4FEF is free general RAM on this board.
FRAME_LO = 0x4C00
FRAME_HI = 0x4C01
SPR_X = 0x4C02
SPR_DIR = 0x4C03

IRQ_VECTOR = 0x40       # I = 0, so the handler address is read from 0x0040


def build_program():
    a = Asm(0)

    # Reset vector.
    a.di()
    a.ld_sp(0x4FF0)
    a.jp("start")

    # Mode-2 vector table entry. I is 0 and the latched vector is 0x40, so
    # the CPU fetches the handler address from 0x0040/0x0041.
    a.at(IRQ_VECTOR)
    a.dw_label("irq")

    a.at(0x0060)
    a.label("start")
    a.xor_a()
    a.ld_i_a()                      # I = 0
    a.im2()
    a.ld_a(IRQ_VECTOR)
    a.out_n(0x00)                   # latch the interrupt vector

    # Clear the board latch, then paint the screen.
    a.xor_a()
    a.ld_mem_a(0x5000)              # IRQ disabled while we set up
    a.ld_mem_a(0x5003)              # flipscreen off

    a.ld_hl("vram_data")
    a.ld_de(0x4000)
    a.ld_bc(0x0400)
    a.ldir()
    a.ld_hl("cram_data")
    a.ld_de(0x4400)
    a.ld_bc(0x0400)
    a.ldir()

    # Sprite attributes: 8 sprites at 0x4FF0, colours spread out.
    a.ld_hl("sprite_attr")
    a.ld_de(0x4FF0)
    a.ld_bc(0x0010)
    a.ldir()

    # Sprite coordinates live in write-only I/O at 0x5060.
    a.ld_hl("sprite_pos")
    a.ld_de(0x5060)
    a.ld_bc(0x0010)
    a.ldir()

    a.xor_a()
    a.ld_mem_a(FRAME_LO)
    a.ld_mem_a(FRAME_HI)
    a.ld_a(0x60)
    a.ld_mem_a(SPR_X)
    a.ld_a(0x01)
    a.ld_mem_a(SPR_DIR)

    a.ld_a(0x01)
    a.ld_mem_a(0x5000)              # enable the VBLANK interrupt
    a.ei()

    a.label("main")
    a.ld_a(0x01)
    a.ld_mem_a(0x50C0)              # kick the watchdog
    a.jr("main")

    # ---- interrupt handler: bump the frame counter, walk sprite 0.
    a.label("irq")
    a.push_af()
    a.push_bc()
    a.push_de()
    a.push_hl()

    a.ld_a_mem(FRAME_LO)
    a.inc_a()
    a.ld_mem_a(FRAME_LO)
    a.jr_nz("no_carry")
    a.ld_a_mem(FRAME_HI)
    a.inc_a()
    a.ld_mem_a(FRAME_HI)
    a.label("no_carry")

    # Bounce sprite 0 horizontally between 0x30 and 0xD0.
    a.ld_a_mem(SPR_DIR)
    a.ld_c(0)                       # c = direction as 0 -> +1, else -1
    a.cp_n(0x01)
    a.jr_z("moving_right")
    a.ld_a_mem(SPR_X)
    a.add_n(0xFF)                   # -1
    a.ld_mem_a(SPR_X)
    a.cp_n(0x30)
    a.jr_nz("store_x")
    a.ld_a(0x01)
    a.ld_mem_a(SPR_DIR)
    a.jr("store_x")

    a.label("moving_right")
    a.ld_a_mem(SPR_X)
    a.add_n(0x01)
    a.ld_mem_a(SPR_X)
    a.cp_n(0xD0)
    a.jr_nz("store_x")
    a.xor_a()
    a.ld_mem_a(SPR_DIR)

    a.label("store_x")
    a.ld_a_mem(SPR_X)
    a.ld_mem_a(0x5061)              # sprite 0 X coordinate

    # Show the low byte of the frame counter as two hex digits next to
    # "FRAME:" so a still frame proves interrupts are actually firing.
    a.ld_a_mem(FRAME_LO)
    a.and_n(0x0F)
    a.add_n(ord('0'))
    a.cp_n(ord('9') + 1)
    a.jr_z("hex_a")
    a.jr("hex_store")
    a.label("hex_a")
    a.ld_a(ord('A'))
    a.label("hex_store")
    a.ld_hl(0x4000 + vram_offset(10, 11))
    a.ld_ind_hl_a()

    a.pop_hl()
    a.pop_de()
    a.pop_bc()
    a.pop_af()
    a.ei()
    a.reti()

    # ---- data
    a.label("sprite_attr")
    # Per sprite: [flags|code<<2, colour]. flags bit0 = flip X, bit1 = flip Y.
    for i in range(8):
        code = i % 4
        flip = (i >> 2) & 3         # sprites 4-7 exercise the flip bits
        a.db((code << 2) | flip, (i * 3 + 1) & 0x1F)

    a.label("sprite_pos")
    # Per sprite: [y, x] in the hardware's own coordinates.
    for i in range(8):
        a.db(0x20 + i * 0x18, 0x60 + (i % 2) * 0x30)

    vram, cram = build_screen()
    a.label("vram_data")
    a.db(*vram)
    a.label("cram_data")
    a.db(*cram)

    return a.link(romimage.PACMAN_ROM_SIZE)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="path to write the ROM image to")
    args = ap.parse_args()

    cpu = build_program()
    gfx = build_gfx()
    prom = build_proms()
    snd = bytes(romimage.PACMAN_SND_SIZE)

    img = romimage.build(cpu, gfx, prom, snd,
                         flags=romimage.FLAG_SYNTHETIC)
    with open(args.output, "wb") as f:
        f.write(img)

    print(f"wrote {args.output}")
    print(romimage.describe(img))
    return 0


if __name__ == "__main__":
    sys.exit(main())
