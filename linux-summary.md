# RA8D1 Linux + Blinka: toolchain summary

One page: where the code came from, how the pieces fit, and which of them a
second board actually needs. The detailed recipe with every trap is
`BUILD.md`; the full process log including dead ends is `ra8d1-linux/notes/`.

## Code sources

Primary upstreams the stack is built from. None of them contain the patches
added along the way (those live in the forks and in `ra8d1-linux/`), but this
is the overall picture of what was picked up and why.

- **[cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima)** — the
  single-header RISC-V rv32ima emulator behind the nommu `rvlinux` app. The
  starting point of the whole project, now the dev/recovery path.
- **[TinyEMU](https://bellard.org/tinyemu/)** (Fabrice Bellard,
  `tinyemu-2019-12-21.tar.gz`) — the RV32 core with Sv32 MMU, S-mode and a
  TLB that became the `tinyemu` app. Chosen over extending mini-rv32ima; it
  benchmarked *faster* with paging on than mini-rv32ima without.
- **[Buildroot](https://gitlab.com/buildroot.org/buildroot.git)** — builds
  the entire guest: cross toolchain, Linux kernel, glibc rootfs, CPython,
  i2c-tools. Base config `qemu_riscv32_virt_defconfig`.
- **[Linux kernel](https://kernel.org)** 6.1.44 (via Buildroot) — the Sv32
  guest kernel, trimmed 25.9 → 6.24 MB, with `guest/pv-io.c` built in.
- **[zephyrproject-rtos/zephyr](https://github.com/zephyrproject-rtos/zephyr)**
  via fork **[mikeysklar/zephyr](https://github.com/mikeysklar/zephyr)**,
  branch `ra8d1/glcdc-64byte-alignment` — the RTOS both board apps run on;
  supplies the Ethernet, OSPI, I2C and GPIO drivers the bridge forwards to.
- **[adafruit/circuitpython](https://github.com/adafruit/circuitpython)** via
  fork **[mikeysklar/circuitpython](https://github.com/mikeysklar/circuitpython)**,
  branch `ra8d1/integration` — carries the west workspace
  (`ports/zephyr-cp/`) the Zephyr apps build inside.
- **[adafruit/Adafruit_Blinka](https://github.com/adafruit/Adafruit_Blinka)**
  9.2.0 (`37a4ff53bb76`) — the CircuitPython API on CPython; extended with
  the `RA8D1_PV` chip and `EK_RA8D1_RVLINUX` board files.
- **[adafruit/Adafruit_Python_PlatformDetect](https://github.com/adafruit/Adafruit_Python_PlatformDetect)**
  3.89.1 (`52f61e494c11`) — board identification; taught to recognise the
  board from the pv bridge's i2c adapter name.
- **[adafruit/Adafruit_Python_PureIO](https://github.com/adafruit/Adafruit_Python_PureIO)**
  1.1.11 (`117f1f19f08a`) — pure-Python `smbus`; the `ioctl(I2C_RDWR)` layer
  Blinka's I2C rides on. Used unmodified.
- **[uclinux elf2flt](https://github.com/uclinux-dev/elf2flt)** (via
  Buildroot's `host-elf2flt`) — bFLT linking for the nommu CPython
  experiments. Its thin RISC-V relocation support is why the nommu path
  stopped at `Py_Initialize` and the MMU path won.

## Build & deploy pipeline (dev machine)

```
   DEV MACHINE (Mac + Docker "br" container)
   ═══════════════════════════════════════════════════════════════════

   Buildroot (qemu_riscv32_virt_defconfig)
     │
     ├──► Linux 6.1.44 Sv32 kernel ─── trim: STRICT_KERNEL_RWX=n (-15.2 MB)
     │      + pv-io.c built in           driver prune          (-4.2 MB)
     │      = Image-pv  6.24 MB  (must fit 8 MB OSPI slot, raw)
     │
     └──► rootfs.ext2 (glibc, CPython 3.11 + ctypes, i2c-tools, smbus)
            │
            + Blinka wheels (blinka, platformdetect, pureio)
            + install.py  ──► RA8D1_PV chip + EK_RA8D1_RVLINUX board files
            + restore trimmed stdlib (email, urllib)
            + /etc/init.d/S99* autorun tests (console trap 17 workaround)
            = rootfs-blinka.ext2  32 MB
            │
            ▼
   ┌─ qemu-system-riscv32 -M virt ────────────────────────────┐
   │  GATE: boots? imports resolve? board files wire up?      │
   │  (BLINKA_FORCECHIP/FORCEBOARD exercises port w/o bridge) │
   └──────────────────────────┬───────────────────────────────┘
                              ▼
   pushimage.py ── TCP :5555 (or UART recovery) ──► OSPI slots
   west build -b ek_ra8d1 ──► zephyr.elf ── flash.sh/probe-rs ──► MCU flash
```

## Runtime stack (board)

```
   EK-RA8D1  (Cortex-M85 @ 480 MHz)
   ═══════════════════════════════════════════════════════════════════

   MCU flash: ONE Zephyr app at a time
   ┌────────────────────────────┐   ┌─────────────────────────────────┐
   │ rvlinux (dev/recovery)     │   │ tinyemu (the real thing)        │
   │  mini-rv32ima, nommu       │   │  TinyEMU RV32 core: Sv32 MMU,   │
   │  telnet :23, RA8LDR :5555  │   │  S-mode, TLB — 15.37 MIPS       │
   │  UART loader fallback      │   │  host-side SBI (no OpenSBI)     │
   └────────────────────────────┘   │  virtio-blk ◄── OSPI window     │
                                    │  pv bridge host side ──► Zephyr │
                                    │    i2c/gpio drivers ──► pins    │
                                    └─────────────────────────────────┘
   OSPI NOR 64 MB (memory-mapped 0x90000000):
   [ app boot ][ kernel 8 MB @0x40000 ][ rootfs 55.5 MB @0x840000 ]
   SDRAM 64 MB @0x68000000  =  guest physical RAM

   ── inside the emulated guest ─────────────────────────────────────
   Linux 6.1.44 (Sv32)  root=/dev/vda (virtio-blk, reads OSPI in place)
     │
     pv-io.c ── probes MMIO 0x11200000 ──► /dev/i2c-0 + gpiochip504
     │
     CPython 3.11 (glibc, working ctypes)
     │
     PlatformDetect ── reads i2c adapter name "EK-RA8D1 paravirt"
     │                 ──► chip=RA8D1_PV, board=EK_RA8D1_RVLINUX
     ▼
     Blinka: board.I2C() ─ busio ─ PureIO ioctl(I2C_RDWR) ─ /dev/i2c-0
                │
                ▼   guest → bridge → Zephyr → silicon → device 0x14  ✓
```

## Critical path for another board

| Piece | Critical? | Per-board work |
|---|---|---|
| **TinyEMU Zephyr app** (MMU emulator + SBI + virtio) | **Yes** — this is what makes Blinka possible at all (`ctypes` needs an MMU) | Port RAM/flash addresses, UART; core is portable C |
| **~64 MB RAM + big storage** | **Yes** — hard floor for glibc+CPython guest | Board selection criterion, not code |
| **pv bridge** (host side in app + `pv-io.c` guest driver) | **Yes** — the only route from guest to real pins | Host side: wire to the board's Zephyr i2c/gpio devices. Guest driver: reusable as-is |
| **Buildroot MMU guest** (kernel trim + rootfs) | **Yes** | Mostly reusable; kernel trim to fit whatever slot exists |
| **Blinka port** (chip + board files, install.py) | **Yes** | Only the pin table and board file are per-board; chip file + detection reuse the adapter-name trick |
| **Image push path** (RA8LDR protocol + pushimage.py) | Practically yes — 10-minute pushes without it are unworkable | Slot table constants |
| mini-rv32ima / rvlinux nommu app | No — the stepping stone; keep as recovery/dev tool | — |
| telnet console | No — nice for dev | — |
| QEMU validation harness | No code, but the discipline: it caught the stdlib gaps and a Blinka pin bug before any 10-minute push | none |

**The one-sentence version:** another board needs the tinyemu app ported to
its memory map, a pv bridge wired to its Zephyr pin/i2c drivers, and a pin
table in the Blinka board file — everything else (guest kernel, rootfs
recipe, `pv-io.c`, detection, push tooling) transfers nearly unchanged.
