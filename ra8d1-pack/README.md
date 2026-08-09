# CircuitPython on the Renesas EK-RA8D1

Working bring-up of CircuitPython (zephyr-cp port) on the Renesas EK-RA8D1
evaluation kit. Four patches, a demo, and the bench tooling used to get there.

**Base commit:** `869d51b20db65ca6836a9a79bd9ab08ff56fcc3c` (adafruit/circuitpython main)
**Board:** EK-RA8D1 (RTK7EKA8D1S01001BE), R7FA8D1BH, Cortex-M85 @ 480 MHz,
2 MB flash / 1 MB SRAM, 64 MB SDRAM, MIPI-DSI 480x854
**Board target:** `renesas_ek_ra8d1` -> `ek_ra8d1/r7fa8d1bhecbd`

---

## What works

| | Verified by |
|---|---|
| CircuitPython boots and runs on the Cortex-M85 @ 480 MHz | banner on panel + live REPL |
| MIPI-DSI panel (ILI9806E) through displayio | photographed rendering a dashboard |
| 64 MB SDRAM available to the CircuitPython heap | `gc.mem_free()` reports ~63 MB |
| REPL over the Zephyr console UART | interactive session, both directions |
| File push to the CircuitPython filesystem, CRC verified | write, reopen, re-read from flash, CRC match |
| FAT filesystem valid to a strict host | image mounts as `DOS_FAT_16`, volume `CIRCUITPY` |
| 4 LEDs, 2 buttons, I2C / SPI / UART / PWM | driven live from `code.py` |

Footprint: FLASH 20.6% (425 KB / 2016 KB), RAM 7.1%, SDRAM 1.2%.

## What does not work

**USB MSC does not mount.** Isolated to the RA8 USB HS bulk-IN data stage.
See `docs/usb-msc-status.md`. Not blocking: the REPL file-push workflow in
`tools/putfile.py` replaces the drive entirely.

---

## The patches

Apply in order to the base commit:

```sh
git checkout 869d51b20
git am patches/*.patch
```

### 0001 — oofatfs: emit a valid boot jump in `f_mkfs`

`f_mkfs()` wrote `0xEB 0xFE 0x90` as the FAT VBR boot jump. `0xEB 0xFE` is
`jmp $`, a two-byte infinite loop. The conventional value is `0xEB 0x3C 0x90`,
a jump over the BPB.

FatFs's own volume check only inspects the first byte, so every board happily
remounts its own volume and the defect goes unnoticed. Hosts that validate the
full jump reject the boot sector and never publish a block device.

**This is not RA8D1-specific — it affects every board using this FatFs to
format its filesystem.** Worth reviewing independently of this port.

### 0002 — boards: ek_ra8d1: use a single GLCDC frame buffer

With two frame buffers, `display_renesas_ra.c` takes the buffer-change path,
sets `vsync_wait`, and blocks on `k_sem_take(frame_buf_sem, K_FOREVER)` at
`display_renesas_ra.c:146`. That semaphore is only given by the GLCDC
line-detect interrupt, which does not fire on this board. **The main thread
blocks forever on the first displayio refresh.**

The failure is silent and total, and it is worth recognising the shape: the
boot banner reaches the panel (the first buffer change lands before the wait),
then the console, USB servicing, `code.py` and the REPL all stop *together*.
Several unrelated subsystems going quiet at the same moment, with a provably
healthy core (`DHCSR` shows no halt/lockup, CFSR/HFSR/DFSR all zero), is one
blocked thread — not several bugs.

A single frame buffer avoids the buffer-change path. The alternative fix is to
bound the wait in the driver (`K_MSEC(100)`), which would preserve double
buffering; that belongs upstream in the Renesas display driver.

### 0003 — zephyr-cp: mirror the console to the chosen console UART

On this board `zephyr,console` is `&uart9`, a board header UART, while the USB
CDC link moves no bytes (see the MSC note). The result is no console at all and
no way to reach the REPL.

The Zephyr chosen console UART is reachable on the on-board debug probe's
virtual COM port — the same cable used for flashing. This mirrors console
traffic there in addition to USB CDC:

- write the UART **first** in `port_serial_write_substring`, so output survives
  a stalled CDC path or a `NULL usb_console`
- drain the UART first in `port_serial_read`
- sum both transports in `port_serial_bytes_available`
- report connected when the mirror is up

Additive — USB CDC remains the primary console where it works. The console RX
buffer goes from 64 to 2048 bytes; 64 overflows during filesystem writes.

This patch is what makes the REPL and file-push workflow possible without USB.

### 0004 — zephyr-cp: propagate `disk_read` failures in the MSC read path

`_zephyr_disk_read()` called `disk_read()` and discarded its return value,
returning 0 unconditionally. A failed read reported success to the host while
handing back whatever was in the buffer. Now returns `-EIO`.

---

## Quick start

```sh
export CP_ROOT=/path/to/circuitpython
export ZEPHYR_SDK=/path/to/zephyr-sdk-1.0.1

tools/build.sh                       # ~4 min
tools/flash.sh                       # ~1 min, west jlink runner

export RA8_CONSOLE=/dev/cu.usbmodemXXXX   # or let the tools autodetect
tools/putfile.py demo/code.py /code.py    # ~90 s, CRC verified
tools/run.py 20                           # soft reset, capture 20 s
```

`tools/` autodetects the first `/dev/cu.usbmodem*` if `RA8_CONSOLE` is unset.
Set `RA8_PROBE` to a `probe-rs` selector (`probe-rs list`) if you have more
than one probe attached.

---

## The demo

`demo/code.py` is a live hardware dashboard on the MIPI panel. Every value is
read from silicon at runtime — part number, clock, UID, firmware version,
panel geometry, heap, filesystem, uptime — with the four LEDs physically
chasing and both buttons polled live.

It also carries a `screendump()` that walks the live displayio group and emits
every element with its coordinates and colour (`demo/screendump-example.txt`).
`tools/render_screendump.py` turns that into a pixel-accurate PNG, which is a
far better record of what is on the glass than a bench photo. Useful for any
headless display work.

Two values are deliberately labelled as stubs on screen:
`microcontroller.cpu.temperature` and `.voltage` are not implemented in this
port — temperature reads a constant `0.0` across repeated samples.

---

## Bench notes worth knowing

**Match the reset to what you are measuring.** A debugger hard reset
(`probe-rs reset`) has no shutdown hook, so CircuitPython's cached dirty
filesystem blocks are discarded. Use the board's own soft reset (Ctrl-D) when
testing write persistence — a hard reset will destroy the file you just wrote
and it looks exactly like a flash bug.

**The J-Link's USB PID changes across a flash.** A pinned probe selector then
fails with "No connected probes were found" — the same message as unplugged.
Re-read from `probe-rs list` after every flash.

**The real Kconfig is `build-<board>/zephyr-cp/zephyr/.config`** (~2900 lines).
The sibling `build-<board>/zephyr/.config` is a sysbuild stub that reports every
symbol as unset; grepping it produces confident false negatives.

**Validate API assumptions before pushing over serial.** A file push takes ~90
seconds. `tools/api_check.py` exercises the displayio/vectorio/terminalio calls
a display program depends on and reports pass/fail per check in a few seconds.

---

## Camera: scoped, not started

`camera = false` in `autogen_board_info.toml` is **not** a driver gap.
`grep -c 'board_info["camera"]' cptools/zephyr2cp.py` returns 0 — the key is
never set, so no board can enable camera regardless of devicetree. Meanwhile
`cptools/compat2driver.py` already maps `renesas_ra_ceu` and
`ovti_ov7670`/`ov2640`/`ov5640` to video.

So the work is **detection plus a `common-hal/camera` module**, not a Zephyr
CEU driver.

Upstream Zephyr already ships a board-specific overlay for this exact kit:
`boards/shields/dvp_20pin_ov7670/boards/ek_ra8d1.overlay` (enables `&pwm3`,
`&cam_clock`, sets `swap-8bits/16bits/32bits` and hsync/vsync sampling), plus
`renesas_aik_ov2640_cam`. The kit connector is the standard
`arducam,dvp-20pin-connector`.

**Switch conflict to settle first:** per the Zephyr board docs, the MIPI LCD
wants SW1-6 GLCD=ON with SW1-7 SDRAM=ON and CAMERA off, while the camera wants
SW1-3 CAMERA=ON with SDRAM=ON and GLCD off. Never enable SW1-4 and SW1-5
together. Whether that is a real electrical conflict or documentation
convention is **unverified here** — check the dts/pinctrl psel sets before
promising a camera-to-display loop demo.

---

## Contents

```
patches/   four git patches, apply with `git am` to the base commit
demo/      code.py dashboard + an example screendump
tools/     build, flash, file push, telemetry probe, API check, renderer
docs/      USB MSC status and the ruled-out theory table
```
