# 01 - Z80 core and Pac-Man machine layer

Date: 2026-08-07. Board: Renesas EK-RA8D1 (`R7FA8D1BH`, Cortex-M85 @ 480 MHz,
64 MB SDRAM @ `0x68000000`, 64 MB octo-SPI NOR memory-mapped at `0x90000000`).

Stage goal was a Z80 core building for the M85, a scaffolded machine layer, a
host harness proving the core, and an honest performance number. Not flashed:
the board is in use, so everything here is host-verified plus a clean target
build.

## Which Z80 core, and why

**`superzazu/z80`**, MIT, vendored at commit `d64fe10` into `emu/z80.c` +
`emu/z80.h`, licence in `emu/LICENSE.z80`.

| | superzazu/z80 | redcode/Z80 | MCUME's core |
|---|---|---|---|
| licence | MIT | GPL-3 (older releases BSD-ish, unclear) | GPL-2 via MAME lineage |
| size | 1 `.c`, 1 `.h`, 26 KB of M85 text | several files, heavy macro layer | tangled into MCUME |
| correctness | passes zexdoc **and** zexall | passes zexall | untested standalone |
| host deps | 5 `fprintf` calls, nothing else | none | assumes MCUME's scaffolding |

MIT is the deciding factor alongside zexall. `redcode/Z80` is a fine core but
its licensing is harder to state cleanly, and the MCUME core inherits MAME's
GPL, which would infect this app. Cycles are counted at instruction
granularity rather than per memory access; Pac-Man has no mid-instruction
timing dependency, so that is not a constraint here.

### Local changes to the vendored core

Two, both purely about building freestanding, neither touching instruction
semantics:

- `z80.h`: dropped `<stdio.h>` (unused there), added `<stddef.h>` for `NULL`.
- `z80.c`: the five `fprintf(stderr, ...)` diagnostics and the body of
  `z80_debug_output()` now sit behind `Z80_TRACE`. The host harness defines
  it; the firmware does not and pulls in no stdio at all.

Verified `-ffreestanding` clean on host and cross-compiled for
`-mcpu=cortex-m85`: **26,004 bytes of text, zero data, zero bss**.

## What was verified

### Z80 correctness

`host/test_z80.c` (upstream's harness, repointed and given timing) runs the
three standard exercisers against the *exact* vendored copy:

```
*** TEST: zexroms/prelim.com
Preliminary tests complete
*** 899 instructions on 8721 cycles (expected=8721, diff=0)

*** TEST: zexroms/zexdoc.cim
*** 5764169747 instructions on 46734978649 cycles (expected=46734978649, diff=0)

*** TEST: zexroms/zexall.cim
*** 5764169747 instructions on 46734978649 cycles (expected=46734978649, diff=0)
```

134 subtests, all `OK`, and the cycle totals match the reference to the
instruction. zexall is the strict one: it checks the undocumented X and Y
flag bits, which is where most hand-written cores fall over.

### Video, orientation and the machine layer

This is the part that could silently be wrong, so it is checked visually.
`host/test_pacman.c` renders the framebuffer to PPM. Running the synthetic
test ROM produces upright, readable text with `TL`/`TR`/`BL`/`BR` in the
correct corners, the score rows at the top, the credit rows at the bottom,
`FRAME: 7` counting up under mode-2 interrupts, and sprite flips visible.

A transpose or a mirror anywhere in the tile decode, the video RAM map or the
rotation shows up instantly as rotated glyphs. It did, on the first attempt:
the encoder in `tools/mktestrom.py` had its portrait indices swapped relative
to the decoder, which the render caught immediately. That is the whole reason
the test pattern is text rather than shapes.

### Container error paths

`romimg_parse()` rejects a corrupted payload (`crc mismatch`), a damaged
magic (`bad magic`) and a truncated file (`region runs past end of image`).

### The zero-copy display contract

`host/test_surface` renders the same machine state twice - once to the
built-in buffer, once into a deliberately wider external one - and requires
the 224x288 window to be byte-identical, nothing outside it to be touched,
and reverting to the built-in buffer to still match. This is the contract the
display layer relies on to have the machine draw straight into its
framebuffer, so it is worth a test rather than an assumption.

All of the above still pass unchanged after the layering rework: same
instruction counts, same T-states, byte-identical render.

### Target build

```
Memory region  Used Size  Region Size  %age Used
       FLASH:    76832 B      2016 KB      3.72%
         RAM:   144072 B       896 KB     15.70%
       SDRAM:        0 B        64 MB      0.00%
```

`pacman_t` is 118 KB and sits in `.bss`, i.e. **internal SRAM**. That is
deliberate and it is the single biggest performance decision here: the Z80's
entire address space is 20 KB and the decoded graphics are 32 KB, so the
emulator's whole hot working set stays on-chip and never touches SDRAM. The
contrast with the emulated-Linux app on this board is stark - that one is
bandwidth-bound on SDRAM precisely because its guest RAM cannot fit anywhere
else.

## Layering

The emulator is meant to be the first instance of a portable shape, not an
RA8D1 program, so board facts are confined to one file and come from
devicetree rather than from constants.

```
emu/                portable C99. Zero Zephyr headers, zero board constants.
                    Compiled unchanged by host/Makefile and by the target.
src/platform.h      the shim: ROM storage, timing, input, console.
src/video.h         the shim: framebuffer, palette, scaling, vsync, present.
src/platform_zephyr.c   the only file that knows the board.
src/video.c             ditto, display stage.
src/main.c          glue. No addresses, no panel size, no driver includes.
```

Audited rather than asserted - `grep` for Zephyr headers per file:

| file | `#include <zephyr/...>` |
|---|---|
| `emu/*.c`, `emu/*.h` | 0 |
| `src/platform.h`, `src/video.h` | 0 |
| `src/main.c` | 0 |
| `src/platform_zephyr.c` | 8 |
| `src/video.c` | 7 |

The only hex constants left in `emu/` are Z80 guest addresses like `0x4800`,
which are part of the machine's own memory map and belong there.

### What moved, and why

- **`pacman_blit_rgb565()` is gone.** A machine layer that knows a panel's
  pixel format is exactly the leak worth avoiding. The machine renders 8-bit
  palette indices - which is what the hardware conceptually produces anyway -
  and `pacman_palette_rgb888()` hands the resistor-ladder colours to the
  display layer in a format-neutral form. Choosing RGB565 or CLUT8 is now the
  display stage's call, and it picked CLUT8.
- **`pacman_set_surface(m, pixels, stride)`** lets the renderer draw straight
  into a platform framebuffer at any stride. This is not just tidiness: it
  removes a 64 KB copy per frame, because `video.h`'s contract is already
  8bpp indexed and matches what `pacman_render()` emits. Without a display
  the machine falls back to its own `m->fb` and the host harnesses work
  unchanged. Verified by rendering into a deliberately wider buffer and
  checking the 224x288 window is byte-identical with no overrun outside it.
- **The ROM slot is a devicetree partition**, `arcade-rom`, declared in
  `app.overlay`. The shim derives the flash device, the XIP window base and
  the slot offset/size from `DT_MTD_FROM_FIXED_PARTITION`, so they cannot
  drift apart. `0x400000` and `0x90000000` no longer appear in any `.c` file.
  Build-time `BUILD_ASSERT`s check *invariants* - erase-block alignment,
  minimum size - rather than specific addresses, since asserting the
  addresses would just move the constants back into C.
- **Erase and write-block handling moved into the shim.** The
  "small `flash_write()` calls fail on this NOR" rule and the hold-the-magic-
  block-back trick are platform properties, so `plat_rom_write()` takes any
  chunk size and buffers internally. The loader in `main.c` no longer knows
  what a 4096-byte block is.

`main.c` guards the video calls with `CONFIG_DISPLAY` and `CMakeLists.txt`
compiles `video.c` only when that is set, so the CPU and display stages can
land independently without either blocking the other.

## Pac-Man hardware, as implemented

Reference: MAME's driver, `src/mame/pacman/pacman.cpp` and `pacman_v.cpp`
(BSD-3-Clause, Nicola Salmoria), used for hardware facts only - register
addresses, PROM wiring, gfx bit layouts, screen timing. `emu/pacman.c` is an
independent implementation, so nothing here inherits MAME's licence.
Cross-checked against the Pac-Man Emulation Guide at
`walkofmind.com/programming/pie/`.

### Memory map

A15 is not wired at the CPU, so the whole map mirrors at `+0x8000`.

| range | contents |
|---|---|
| `0x0000-0x3FFF` | 16K program ROM |
| `0x4000-0x43FF` | video RAM, tile codes |
| `0x4400-0x47FF` | colour RAM, tile attributes |
| `0x4800-0x4BFF` | unmapped, floats at `0xBF` |
| `0x4C00-0x4FEF` | work RAM |
| `0x4FF0-0x4FFF` | sprite RAM, 8 x `[flags+code, colour]` |
| `0x5000-0x5007` | w: 74LS259 latch, D0. r: IN0 |
| `0x5040-0x505F` | w: Namco WSG sound. r: IN1 |
| `0x5060-0x506F` | w: sprite coordinates, 8 x `[y, x]`, write-only |
| `0x5080` | r: DSW1 |
| `0x50C0` | w: watchdog kick. r: DSW2 |

Reads in `0x5000-0x50FF` decode only bits 6-7 of the low byte. The 74LS259
outputs are Q0 IRQ mask, Q1 sound enable, Q3 flipscreen, Q7 coin counter.

I/O space has exactly one decoded port: **`OUT (0x00),A` latches the mode-2
interrupt vector**. The CPU runs in IM 2 and that latch supplies the vector
byte on acknowledge. This is easy to miss and the game does not boot without
it.

### Timing

Z80 at 18.432 MHz / 6 = **3.072 MHz**. Video 6.144 MHz dot clock, 384x264
total, 288x224 active, **60.606 Hz**. That gives **192 T-states per scanline**
and **50688 per frame**, with VBLANK starting at line 224, i.e. 43008
T-states in. `pacman_run_frame()` runs to 43008, raises the interrupt if the
mask bit is set, runs the remaining 7680, then renders.

### Orientation

The raster is 288x224 landscape and the monitor is bolted in sideways. Rather
than render landscape and rotate afterwards, the rotation is folded into the
graphics decode and into the tile lookup table:

```
landscape (sx, sy)  ->  portrait (px, py) = (223 - sy, sx)
```

so the framebuffer comes out **224x288 portrait with no second pass**. This
matters more than it sounds: a post-rotation pass over 64512 bytes every
frame is pure waste, and on this board the panel is portrait anyway.

The video RAM layout is the awkward part. The middle 28 columns are stored
row-major in 32-byte strides; the two columns at each end - which become the
score row at the top of the portrait screen and the credit row at the bottom -
live in the leftover space at the start and end of the 1K page. The signed
wrap in `build_vram_map()` (`c = -1` masking to 31, `c = -2` to 30) is exactly
what the address decode does and is load-bearing.

### Graphics decode

Both gfx layouts pack two bitplanes for four pixels into one byte, MSB-first:
for the k'th pixel of a group, plane 0 is data bit `7-k` and plane 1 is bit
`3-k`. Tiles are 8x8 in 16 bytes; sprites 16x16 in 64 bytes.

All 256 tiles and 64 sprites are **decoded once at load time** into one byte
per pixel, already rotated. 32 KB of SRAM buys a render loop that is a plain
byte copy with a colour-table indirection, instead of bit-unpacking 64512
times per frame.

### Colour

The 82s123 palette PROM drives a resistor ladder: bits 0-2 red and 3-5 green
through 1k/470/220 ohm, bits 6-7 blue through 470/220. Normalising
conductance ratios so all-on is 255 gives weights `{33, 71, 151}` for red and
green and `{81, 174}` for blue, both summing to exactly 255, so no clamping is
needed. The 82s126 lookup PROM is 64 palettes of 4 entries, low nibble.
Sprite pixels whose lookup yields 0 are transparent.

## Performance

### Measured, on host (Apple silicon, this Mac)

Five runs of `host/bench`, 5000 frames each, synthetic test ROM:

| | |
|---|---|
| Z80 instructions | 4753 per frame |
| T-states | 50694 per frame (nominal 50688) |
| CPU emulation | **~150 M Z80 instructions/s** |
| render 224x288 | **~23 us/frame** |

Run-to-run spread was under 5% once the machine was otherwise idle. An early
reading of 35 M instr/s was taken under contention and is not representative.

For reference, the same core through zexall's plain-array bus does 179 M
instr/s, so the function-pointer bus in `pacman_t` costs about 16%. If the
target ever needs it, inlining the memory accessors into the core is the
obvious first optimisation.

### Estimated, on the M85 - and the caveats

There is a genuinely useful calibration point available: the emulated-Linux
work on **this exact board** measured `mini-rv32ima` at ~16.7 M guest
instructions/s on the M85 (35-40 M instructions to boot, 2.4 s), against
44.6 M guest instructions/s for the same interpreter **on this same Mac**.
That is a measured host-to-target ratio of **~0.35x** for an interpreter
workload.

Applying it: `150 M x 0.35` = **~52 M Z80 instructions/s**.

Pac-Man needs 3.072 M T-states/s. Real Z80 game code averages roughly 5.5-6
T-states per instruction, so **~0.55 M instructions/s**. That is
**~90x real time**.

Being deliberately pessimistic - assume the ratio is three times worse than
measured, 0.1x, because the Z80 core is branchier than `mini-rv32ima` and the
M85 is in-order - still gives 15 M instr/s, i.e. **~27x real time**.

So the floor is around 25x and the likely figure is near 90x. Both are well
above the 3-8x this stage was scoped against. Two reasons the ratio should
actually be *better* than 0.35x for us: `mini-rv32ima`'s guest RAM is in
SDRAM and every guest fetch pays for it, whereas our entire Z80 address space
is 20 KB of internal SRAM; and there are no MMU walks or traps in this
workload.

**Caveats, stated plainly.** This is an extrapolation, not a board
measurement - the board was not to be flashed at this stage. And the
benchmark workload is the *synthetic* test ROM, whose main loop is three
instructions and whose 10.67 T-states/instruction is not representative of
real game code. `src/main.c` contains the identical benchmark, so the moment
someone does flash this the real number drops out of the console.

### The thing that actually binds: getting pixels to the panel

The Z80 is not the constraint. The display path is.

My first cut materialised a 3x RGB565 buffer: 672x864 = 1.16 MB per frame,
which at 60.6 Hz is **70 MB/s of SDRAM writes** against the **150 MB/s**
measured on this board - about half the frame budget, versus a few percent
for CPU and render combined. That blit has since been deleted, for two
reasons. It baked a pixel format into the portable machine layer, and the
display stage independently reached a better answer: CLUT8 direct GLCDC
scanout at 2x, ~2 ms per frame, no per-frame scaling pass at all. See
`notes/02-video.md`.

The finding that survives is the shape of the problem: **do not materialise a
scaled copy every frame.** Either let the display controller scan out the
native 224x288 indexed buffer, or expand a scanline at a time. Also worth
recording, since it caught me out: 3x is 672x864 and does not fit the 480x854
panel; 2x does.

## ROMs

**No arcade ROM is shipped, fetched or committed.** Pac-Man's ROMs are
copyrighted. The ROM source is a documented input:

- `tools/mktestrom.py` generates a complete synthetic ROM set from scratch -
  program, tiles, sprites and both PROMs. Nothing in it derives from a real
  dump, so it is free to commit and hand around. This is what everything above
  was tested with.
- `tools/mkromimage.py` packages a set **you** already own, from a directory
  or a MAME-layout zip. It verifies each member's SHA-1 against MAME's
  recorded digests so a bad dump is reported rather than producing a
  confusingly broken machine. Accepts both `pacman` and `puckman` layouts.
- `tools/pushrom.py` sends the image to the board over the console UART.

Images use a flat 64-byte-header container (`RA8ARC01`, see
`tools/romimage.py`) with a CRC-32. The firmware parses it **in place** in the
memory-mapped NOR at `0x90400000` and points the decoder straight at it - no
copy. `0x400000` was chosen to clear the CIRCUITPY filesystem, the OSPI
autocalibration sector and the emulated-Linux image.

The UART loader writes in 4096-byte aligned blocks because small
`flash_write()` calls fail on this NOR (`ra8d1-linux/ra8d-bringup.md`), and
writes the first block - the one holding the magic - **last**, so a truncated
transfer never leaves something that parses.

## What is not done

- **Sound.** The Namco WSG register writes are captured into
  `m->sound_regs` but nothing synthesises them. The 3-voice WSG needs the
  256-byte waveform PROM, which the container already carries.
- **Display.** Owned by the display stage (`src/video.c`,
  `notes/02-video.md`). The integration point is in place: when
  `CONFIG_DISPLAY` is set, `main.c` binds the machine's render surface to
  `plat_get_framebuffer()` and pushes the palette through
  `plat_set_palette()`, so the machine draws straight into the display
  buffer with no copy. Until then the app runs headless and `video.c` is not
  compiled in.
- **Input.** The path is complete and wired end to end - `plat_poll_input()`
  returns a `PLAT_BTN_*` mask, `main.c` maps it onto `pacman_set_input()` -
  but the Zephyr implementation returns 0 because no GPIO or USB HID source
  is attached yet. The board's two user buttons are the obvious first move,
  and it is a change to `platform_zephyr.c` only.
- **On-board measurement.** Everything is host figures plus a clean build.
- **Cocktail flipscreen** mirrors the tilemap but not sprites. This matches
  MAME's behaviour and is irrelevant for an upright cabinet, but it is a
  known simplification rather than an oversight.
- The `pacman_read_nop` value `0xBF` is prefilled into `0x4800-0x4BFF` at
  reset so the fast read path stays a single index. MAME notes that what the
  floating bus actually returns is not fully pinned down; Ms. Pac-Man is
  sensitive to it, plain Pac-Man is not.

## Build and run

```sh
# host: correctness, render, throughput
cd host
make test        # prelim + zexdoc + zexall, ~70 s
make render      # generates the synthetic ROM, writes frame.ppm
make surface     # zero-copy render-into-platform-framebuffer contract
make runbench

# target: build only
source /Users/sklarm/Downloads/ada/siwx917/env.sh
source /Users/sklarm/Downloads/ada/siwx917/.venv/bin/activate
cd /Users/sklarm/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp
west build -p always -b ek_ra8d1 \
  /Users/sklarm/Downloads/ada/siwx917/ra8d1-arcade \
  -d /Users/sklarm/Downloads/ada/siwx917/ra8d1-arcade/build
```

`flash.sh` is present and is the rvlinux one unchanged in substance: probe-rs,
option-setting sections stripped, and **not** `west`/`JLinkExe`, which
force-update the on-board probe firmware and brick it.
