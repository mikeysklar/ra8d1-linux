# Linux, CPython and Adafruit Blinka on the Renesas EK-RA8D1

A riscv32 Linux guest — Sv32 MMU, glibc, CPython 3.11 with working `ctypes` —
running inside a RISC-V emulator on the board's Cortex-M85, with Adafruit
Blinka driving the board's real I2C and GPIO pins through a paravirtual
bridge. `ssh root@<guest>` works. `pip install` works. The stock Adafruit
`blinkatest.py` passes.

The point of the exercise: measure what 64 MB of SDRAM and 64 MB of flash buy
over the usual MCU constraints. The answer turned out to be "an actual Linux
computer", at 15.37 MIPS.

```
$ ssh root@192.168.2.4
# python3 ./blinka_test.py
BLINKA PASS detect: chip=RA8D1_PV board=EK_RA8D1_RVLINUX
BLINKA PASS import-board: board module ok, SCL=<RA8D1 bus pin P512> SDA=<RA8D1 bus pin P511>
BLINKA PASS i2c-scan: scan=['0x14']
BLINKA TEST DONE
```

## Reading order

| file | what |
|---|---|
| `BUILD.md` | **the recipe** — every step from a stock board, and 27 numbered traps so each is only hit once |
| `usage.md` | **driving it** — how this differs from Blinka on a Raspberry Pi |
| `linux-summary.md` | one page: where the code came from, how the pieces fit, what a second board would need |
| `ra8d1-tinyemu/` | the Zephyr host app: TinyEMU RV32 core, telnet console, guest NIC bridge, TCP image pusher |
| `ra8d1-linux/` | guest side: the `pv-io.c` bridge driver, the Blinka port, Buildroot recipes, and `notes/` — the full research record including the dead ends |

## The shape of it

```
EK-RA8D1 (Cortex-M85 @ 480 MHz)
└── Zephyr app (ra8d1-tinyemu): TinyEMU RV32 emulator, Sv32 MMU
    ├── OSPI NOR 64 MB: kernel slot + 55 MB rootfs, read in place (never copied)
    ├── SDRAM 64 MB: guest physical RAM
    ├── eth0 shared: host stack (telnet :23, pusher :5555)
    │                + promiscuous bridge → guest's virtio-net NIC
    └── guest: Linux 6.1 → CPython 3.11 → Blinka
                └── pv-io.c → MMIO bridge → Zephyr I2C/GPIO → real pins
```

Highlights of what it took, each with its full story in `BUILD.md`'s traps or
the `notes/` files:

- **`ctypes` needs an MMU**, which is why this is TinyEMU with Sv32 rather
  than a nommu core — Buildroot gates python3 on `BR2_USE_MMU`, and the nommu
  bFLT experiments died exactly where the notes predicted.
- **The rootfs is never copied.** virtio-blk reads it in place from the
  memory-mapped OSPI window; that is the only reason 55 MB of filesystem fits
  a board whose RAM the kernel already wants a third of.
- **Console input was silently dropped for a day** (trap 17): the guest polls
  its UART once per emulator slice, so a pasted line lost its newline and the
  shell blocked forever. Fixed with interrupt-driven RX for 452 bytes.
- **Blinka detection is automatic** — PlatformDetect reads the bridge's i2c
  adapter name, so stock examples run unmodified, no env vars.
- **The guest is a real network citizen**: its own MAC, its own DHCP lease,
  dropbear SSH. The Zephyr Ethernet driver needed promiscuous-mode support
  written (fork `mikeysklar/zephyr`, issues #10/#11 carry the trail).
- **The flash and TCP layers fought back**: a status-poll race that could
  corrupt every block written, a TCP receive window smaller than one MSS, an
  ECC rule that forbids reprogramming 16-byte units. Traps 25-27.

## Hardware

```sh
probe-rs list        # SEGGER J-Link, 1366:0105
screen /dev/cu.usbmodem<serial>1 921600      # console via J-Link VCOM
```

SW1-5 ON for Ethernet (SDRAM still works, verified — the vendor table
implying a conflict is wrong). Build environment and flashing are in
`BUILD.md` §0-§1.
