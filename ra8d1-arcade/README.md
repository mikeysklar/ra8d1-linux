# ra8d1-arcade

Namco Pac-Man arcade hardware emulated natively on a Renesas EK-RA8D1
(Cortex-M85 @ 480 MHz), as a Zephyr app. 224x288 portrait, which is the point:
the board's panel is 480x854 portrait and Pac-Man's cabinet was too.

This runs *directly* on the M85. It is not hosted inside the emulated RISC-V
Linux that also lives on this board - nesting an M85 emulating a RISC-V
emulating a Z80 costs well over a thousand host cycles per Z80 instruction,
roughly ten times too slow. Direct emulation costs tens of cycles.

See `notes/01-core.md` for the hardware map, what was verified, and the
performance numbers.

## No ROMs

**No arcade ROM is included, downloaded or committed.** They are copyrighted.
The ROM set is an input you supply.

For testing there is a complete **synthetic** ROM set generated from scratch -
program, tiles, sprites and both PROMs - which is what the whole pipeline was
developed and verified against:

```sh
python3 tools/mktestrom.py testrom.bin
```

To use a set you already own, from a directory or a MAME-layout zip:

```sh
python3 tools/mkromimage.py ~/roms/pacman.zip pacman-image.bin
```

Each member's SHA-1 is checked against MAME's recorded digests, so a bad dump
is reported rather than producing a confusingly broken machine.

## Layout

```
emu/                portable C99 - no Zephyr, no board, no libc beyond
                    memcpy/memset. Compiles unchanged on the Mac and on target.
  z80.c/.h            vendored superzazu/z80 @ d64fe10, MIT, passes zexall
  pacman.c/.h         machine layer: memory map, video, sprites, palette, IRQ
  romimage.c/.h       ROM container parser, parses in place, no copy
src/platform.h      the shim: ROM storage, timing, input, console
src/video.h         the shim: framebuffer, palette, scaling, vsync, present
src/platform_zephyr.c   the only file that knows what board this is
src/video.c             ditto, display stage
src/main.c          glue: no addresses, no panel size, no driver includes
host/               test harnesses that build and run on the Mac
tools/              ROM image generation and the board pusher
notes/              write-ups
```

The layering is deliberate: this is meant to be portable to another Zephyr
board by writing a devicetree overlay and a shim implementation, not by
editing the emulator. Board facts - the flash device, its memory-mapped
window, the ROM slot offset and size - come from devicetree via
`DT_MTD_FROM_FIXED_PARTITION` on the `arcade-rom` partition declared in
`boards/ek_ra8d1.overlay`. No `.c` file contains `0x90000000` or `0x400000`.

Board-specific configuration is split out so that porting is a matter of
adding files, not editing them:

```
prj.conf                  board-neutral Kconfig
boards/ek_ra8d1.conf      this board's Kconfig, merged on top of prj.conf
boards/ek_ra8d1.overlay   this board's devicetree
```

There is deliberately no `app.overlay`. Zephyr selects the application
overlay by a fallback chain rather than merging: once `boards/<board>.overlay`
exists, `app.overlay` is never read, so keeping both would leave one of them
silently doing nothing. Kconfig is the opposite - `prj.conf` and
`boards/<board>.conf` are merged - which is why the two are split the way they
are above.

## Host

```sh
cd host
make test        # prelim + zexdoc + zexall against the vendored core (~70 s)
make render      # generates the synthetic ROM, renders frame.ppm
make surface     # guards the zero-copy contract with the display layer
make runbench    # Z80 instructions/s and render cost
```

`make render` is the orientation check: the test pattern is text, so any
transpose or mirror in the tile decode, the video RAM map or the portrait
rotation is immediately visible.

## Target

Build only - this does not flash.

```sh
source ~/Downloads/ada/siwx917/env.sh
source ~/Downloads/ada/siwx917/.venv/bin/activate
cd ~/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp
west build -p always -b ek_ra8d1 --shield rtkmipilcdb00000be \
  ~/Downloads/ada/ra8d1/ra8d1-arcade \
  -d ~/Downloads/ada/ra8d1/ra8d1-arcade/build
```

The shield is what supplies the panel: it sets `chosen zephyr,display`, and
the bare board DTS has `glcdc` and `dsihost` both disabled. Drop `--shield`
and the app still builds, headless - `video.c` is compiled in only when a
`zephyr,display` is chosen, which `CMakeLists.txt` turns into
`ARCADE_HAS_DISPLAY` for `main.c`. That is how the emulator and the display
stage stay separable, and it is useful for isolating problems that have
nothing to do with video.

Then, when you do want it on hardware, `./flash.sh` (probe-rs; do **not** use
`west flash` or `JLinkExe` on this board - they force-update the on-board probe
firmware and brick it), and push a ROM image with:

```sh
python3 tools/pushrom.py /dev/tty.usbmodemXXXX testrom.bin
```

The image lands in the octo-SPI NOR at offset `0x400000` and survives resets
and app reflashes.

## State

Working: Z80 core (zexall clean), full video path, sprites, palette PROMs,
mode-2 interrupts, watchdog, DIP switches, ROM container and UART loader,
target build.

Not done: sound (WSG register writes are captured but not synthesised),
display output (the scaled buffer is produced and left in SDRAM), input wiring,
and any measurement on the board itself.

## Licences

`emu/z80.c` and `emu/z80.h` are MIT, (c) 2019 Nicolas Allemand - see
`emu/LICENSE.z80`. Everything else here is original. MAME's Pac-Man driver was
read as a hardware reference only; no MAME code was copied, so nothing here
inherits its licence.
