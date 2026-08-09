# ra8d1-tinyemu

TinyEMU's MMU-capable RV32 core, ported to a Zephyr application for the
Renesas EK-RA8D1, so the board can run an **Sv32** riscv32 Linux guest instead
of the nommu one in `../ra8d1-linux`.

The point of the MMU is not the MMU: Buildroot gates `python3` on
`BR2_USE_MMU`, so an MMU guest is the shortest path to Blinka on this board.
See `../ra8d1-linux/notes/sv32-mmu.md` for how that was decided and
[`notes/00-port.md`](notes/00-port.md) for the port itself.

**Status: Linux runs on the board.** rv32 Sv32 Linux 6.1.44 boots from OSPI
flash to a `buildroot login:` prompt in **34.5 s from reset**, and
`python3 -c "import ctypes"` works there. The emulator does **15.37 MIPS with
Sv32 paging active** on the M85.

What is not done: the paravirt I/O bridge, so nothing in the guest has driven
real RA8D1 hardware yet.

```
FLASH:  90968 B / 2016 KB   4.41%
RAM:    25560 B /  896 KB   2.79%
```

`TINYEMU_FLEN` defaults to 64 because the Buildroot guest userspace is built
for the ilp32d ABI; at 0 the kernel boots fine and init dies with SIGILL. See
[`notes/00-port.md`](notes/00-port.md) §6.

## Layout

| dir | what |
|---|---|
| `emu/` | portable C99. TinyEMU's core plus our machine layer, SBI and devicetree generator. Knows nothing about Zephyr or this board. |
| `src/` | the Zephyr shim. `platform_zephyr.c` is the only file that knows what board this is. |
| `host/` | the same `emu/` built for macOS, plus the RV32 test payload |
| `guest/` | the trimmed guest kernel config, recipe and rationale |
| `tools/` | regenerates the compiled-in RV32 self-test |
| `boards/` | devicetree overlay and Kconfig for `ek_ra8d1` |

## Build

```sh
source ~/Downloads/ada/siwx917/env.sh
source ~/Downloads/ada/siwx917/.venv/bin/activate
cd ~/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp   # the west workspace
west build -p always -b ek_ra8d1 ~/Downloads/ada/ra8d1/ra8d1-tinyemu \
  -d ~/Downloads/ada/ra8d1/ra8d1-tinyemu/build
```

`./flash.sh` downloads it with probe-rs. Do **not** use `west flash` or
`JLinkExe` on this board — see the note in the script.

## Host development loop

Much faster than flashing, and it exercises exactly the same `emu/` sources:

```sh
cd host && make                 # build tinyemu-host
cd tests && make                # build the RV32 payload (needs the `br` container)
../tinyemu-host smoke.bin       # run it
```

```sh
# and a real guest
./tinyemu-host Image -r rootfs.ext2 \
    -c "console=ttyS0,115200 earlycon=sbi root=/dev/vda ro rootfstype=ext2"
```

Useful flags: `-r <rootfs>` (attached as read-only virtio-blk `vda`),
`-c "<cmdline>"`, `-n <insn limit>`, `-t <seconds>`, and `-d out.dtb` to dump
the generated devicetree for `dtc -I dtb -O dts`.

## Guest images

Two flash slots, in the format the `ra8d1-linux/rvlinux` app already writes and
its TCP pusher already tests: kernel 8 MB at `0x40000` (magic `RA8LINUX`),
rootfs 55.75 MB at `0x840000` (magic `RA8ROOTF`). This app **reads** those
slots; it has no writer of its own, so pushing is done with rvlinux's tool.

The rootfs is read in place through the memory-mapped OSPI window and never
copied into SDRAM — which is what makes a 55 MB root filesystem possible on a
board with 64 MB of RAM the kernel already wants 26 of.

With an empty kernel slot the app boots a compiled-in RV32 self-test instead,
so a freshly flashed board says something true about the emulator without
anyone having to push an image first.

Note: nothing here decompresses, so the raw 25.9 MB Image does not fit the
8 MB kernel slot yet. See [`notes/00-port.md`](notes/00-port.md) §10 item 1.

## Licence

`emu/riscv_cpu*.{c,h}`, `emu/softfp*.{c,h}`, `emu/cutils.h` and `emu/iomem.h`
are Fabrice Bellard's TinyEMU 2019-12-21, MIT — see `emu/LICENSE.tinyemu`. `emu/rv_iomem.c` is
derived from its `iomem.c` and carries the same licence. Everything else here
is ours.
