# Linux on the Renesas EK-RA8D1 — bring-up record

**Status: working.** RISC-V Linux 6.1.14 boots to a root shell on a Cortex-M85
in **2.4 seconds**, via emulation, out of the board's 64 MB SDRAM.

Date: 2026-08-07. Board: Renesas EK-RA8D1 (`R7FA8D1BH`, Cortex-M85 @ 480 MHz,
2 MB internal flash, 1 MB SRAM, 64 MB external SDRAM, 64 MB octo-SPI NOR).

```
=== rv32ima Linux on EK-RA8D1 ===
core 480 MHz, sdram 64 MB @ 0x68000000
image ok, 3476752 bytes
copy 3476752 B flash->sdram... ok 501 ms
booting guest...

[    0.000000] Linux version 6.1.14 (riscv32-buildroot-linux-uclibc-gcc 12.2.0)
[    0.000000] Machine model: riscv-minimal-nommu,qemu
[    0.000000] Memory: 61384K/65532K available
[    0.000000] clint: timer running at 1000000 Hz
[    2.433643] Run /init as init process

Welcome to Buildroot
buildroot login: root

~ # cat /proc/cpuinfo
isa  : rv32ima
mmu  : none
~ # free
              total    used    free
Mem:          62848    2900   57584
```

---

## Why emulation, not native

Native Linux on this silicon is impossible, on two independent grounds:

```
upstream arch/arm NOMMU   ARMv7-M ONLY. Zero Armv8-M / Armv8.1-M support.
                          A 2022 consolidation removed ARMv4/v5 and nothing
                          has been added since. Cortex-M85 is Armv8.1-M.
Cortex-M85 Linux          nothing anywhere: no port, no patch series, no
                          attempt on LKML, GitHub, or in ARM's own docs.
RA8-family Linux          nothing. Renesas FSP offers FreeRTOS, Azure RTOS
                          and Zephyr only.
uClinux precedent         exists for M3/M4/M7. Nothing for M85.
ST's own Cortex-M85       STM32V8 (Nov 2025) also has no Linux.
```

It is deliberate product segmentation, not an oversight: Renesas sells RZ/A,
RZ/G and RZ/V with Cortex-A and real MMUs for Linux workloads.

`jserv/cortexm-linux` looks like an on-ramp and is not — it targets Cortex-M4
on **QEMU MPS2-AN386**, not silicon, and calls itself a testbed.

Emulating a RISC-V gives the guest a **real MMU**, which is the practical
argument as much as the novelty: nommu Linux fails on *contiguity*, not
capacity, and that failure mode is what bites real no-MMU ports.

## What was measured, and why it matters

Every prior `mini-rv32ima` MCU port drives **SPI PSRAM**. This board has
**parallel SDRAM** on the memory bus. That turned out to be the whole story.

| board | clock | memory | boot to login |
|---|---|---|---|
| RP2040 `pico-rv32ima` | 133 MHz | 8 MB SPI PSRAM | ~30 s |
| ESP32-C3 `uc-rv32ima` | 160 MHz | 8 MB SPI PSRAM | ~80 s |
| ESP32-P4 | 240 MHz | 32 MB PSRAM | 17.9 s |
| **EK-RA8D1** | **480 MHz** | **64 MB parallel SDRAM** | **2.4 s** |

**7.5x faster than the ESP32-P4 at 2x the clock.** Clock scaling alone predicts
~9 s. So the existing ports are **bandwidth-bound, not clock-bound**, and
parallel SDRAM is worth roughly **4x beyond what clock explains**.

That is the result worth reporting. It could not be answered from a datasheet,
and an earlier 10-15 s extrapolation scaled from the ESP32-P4 was wrong in the
conservative direction — because it assumed clock was the binding constraint,
which is the exact thing the experiment existed to test.

Supporting measurements, all on hardware:

```
SDRAM sequential read     123 MiB/s cached   32 MiB/s uncached   (3.8x)
SDRAM sequential write    150 MiB/s          cache-independent
SDRAM unaligned u32 read  128 MiB/s          same as aligned
memcpy                     31 MiB/s          weak spot, hand-written loop
                                             likely worth a couple of x
image load 3.4 MB         501 ms             memory-mapped OSPI -> SDRAM
emulator code size          2,316 B  -Os     Cortex-M85, zero libgcc calls
app total                  52 KB flash / 19.7 KB RAM
```

## Claim, narrowed

**Not** "first emulated Linux on a Cortex-M" — `tvlad1234/pico-rv32ima` has run
mini-rv32ima on the RP2040 (dual Cortex-M0+, no RISC-V core) since 2023.

What survives: **first on Armv8.1-M / Cortex-M85**, and **first on parallel
SDRAM rather than SPI PSRAM**. The bandwidth result is the substantive part.

Two research gaps remain open and are not claimed as closed: the LKML
kernel-side sweep, and Gitee / Chinese-language sources.

---

## How it works

```
0x68000000  64 MB SDRAM   guest RAM (guest sees it as 0x80000000)
0x90000000  64 MB OSPI    memory-mapped NOR, CS1
  +0x00000                CIRCUITPY filesystem (128 KB)
  +0x02000                OSPI driver autocalibration sector — do not touch
  +0x40000                our image header (own 4 KB block)
  +0x41000                kernel image, 3,476,752 bytes
```

**The emulator is `mini-rv32ima.h` used directly with its stock flat
accessors.** No custom memory bus, no callbacks, no HAL. SDRAM is directly
addressable, so `MINIRV32_STORE4`/`LOAD4` compile to single instructions
against a plain pointer. This is why the SDRAM path was cheap rather than the
significant work it was expected to be — `tiny-rv32ima`'s SPI-PSRAM HAL, which
the other MCU ports use, is not in the path at all.

The host supplies only three things: a microsecond elapsed-time value, a
2-address 8250 UART, and CLINT + syscon MMIO.

Guest boot state, reproduced from the reference implementation:

```c
core->pc       = 0x80000000;
core->regs[10] = 0;                     /* a0 = hart id  */
core->regs[11] = dtb_ptr + 0x80000000;  /* a1 = DTB phys */
core->extraflags |= 3;                  /* machine mode  */
```

DTB is compiled into the app (`default64mbdtc.h`); its RAM-size field at byte
offset `0x13c` carries sentinel `0x00c0ff03` and **must** be patched or the
guest walks past real memory into the DTB and emulator state.

## Files

```
rvlinux/src/main.c        507 lines: image check, UART loader, guest boot,
                          emulated UART/CLINT/syscon, step loop
rvlinux/prj.conf          CONFIG_MEMC, CONFIG_FLASH, CONFIG_CRC, D/I-cache
rvlinux/flash.sh          strip .option_setting* then probe-rs download
emulator/                 cnlohr/mini-rv32ima @ 84858f5
image/Image               3,476,752 B, sha256 5f596134705d5aa8...
                          linux-6.1.14-rv32nommu-cnl-1, initramfs embedded
notes/01-emulator-and-image.md   contract, host boot proof, RAM sweep
notes/02-zephyr-skeleton.md      SDRAM verification and throughput
notes/03-image-storage.md        OSPI analysis, SD-conflict verdict
notes/host-boot.log              host-side boot to shell
notes/ramsweep/                  per-size boot logs
```

## Reproducing

```sh
source ~/Downloads/ada/siwx917/env.sh
source ~/Downloads/ada/siwx917/.venv/bin/activate
cd ~/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp

west build -p always -b ek_ra8d1 ~/Downloads/ada/siwx917/ra8d1-linux/rvlinux \
  -d ~/Downloads/ada/siwx917/ra8d1-linux/rvlinux/build

~/Downloads/ada/siwx917/ra8d1-linux/rvlinux/flash.sh
screen /dev/cu.usbmodem0010868598391 115200      # login: root, no password
```

The image persists in OSPI across resets and app reflashes. It only needs
pushing once (see the loader section below).

---

## Things that cost time

**Guest RAM floor is 11 MB, and 10 MB is a trap.** Swept with the same image:
64/32/16/12/11 MB all boot to a working shell. At 10 MB you reach a login
prompt and it echoes your commands, then `binfmt_flat: Unable to allocate RAM
for process text/data, errno -12` and `Segmentation fault`. Any check that only
greps for a prompt reports a false pass. 9 MB panics, 8 MB OOMs. The cause is
nommu needing *physically contiguous* blocks — at 10 MB there was 3,432 kB free
but the largest block was 512 kB. `pico-rv32ima`'s advertised 8 MB is not
achievable with this image; they use a much smaller userspace.

**Unaligned access was the risk that did not materialise.** The emulator issues
unaligned 32-bit loads, and Cortex-M85 permits those only against Normal-typed
memory. Nothing maps `0x68000000`, so the ARMv8-M default background map types
it Normal WBWA and unaligned runs at full speed. **If anyone later adds an MPU
region over guest RAM it must be typed Normal, never Device** — Device typing
would hard-fault every unaligned guest access.

**Three self-inflicted loader bugs, each costing a 6-minute transfer:**

1. *No erase handshake.* The board spends ~30 s erasing 3.5 MB of NOR and does
   not buffer the UART meanwhile, so the first 4 KB vanished into a full FIFO.
   Fixed with an explicit ready marker.
2. *A single-character `R` ready marker matched the **R** in "LOADER"* in the
   board's own log output, so the host began streaming mid-erase. Changed to
   `<RDY>`. Same bug class Hermes hit independently the same day, with a `risc`
   pattern matching inside "RISC-V".
3. *A 16-byte header write failed where 848 x 4 KB writes had succeeded.* See
   below — this one is not our bug.

**Debugging lesson worth generalising:** print the return code on the *first*
failure. Each blind retry here cost a full 6-minute transfer, and adding one
`udec(-rc)` would have collapsed three attempts into one.

## The small-write bug (undocumented, unreported)

```
flash_erase(nor, 0x40000, 3670016)          OK
flash_write(nor, 0x40010, buf, 4096) x848   OK      every one
flash_write(nor, 0x40000, &hdr, 16)         FAILS   16 B, 16 B-aligned
```

Backwards from intuition. Root cause spans three layers and is stated correctly
by none of them:

```
ek_ra8d1.dts        write-block-size = <1>   advertises "any size is legal"
Zephyr flash_write  no validation            issue #4276 closed Won't Fix,
                                             "drivers check internally" — this
                                             one does not, it delegates to FSP
Renesas FSP         R_OSPI_B_Write(): "without DMAC, write length must be a
                    multiple of CPU access size"  <- the real rejection, inside
                                                     a compiled vendor blob
```

**Fix — pad the header into its own full aligned block:**

```c
#define IMG_PAYLOAD_OFF 4096U
memset(buf, 0xFF, sizeof(buf));       /* 0xFF = erased state */
memcpy(buf, &h, sizeof(h));
flash_write(nor, IMG_FLASH_OFF, buf, IMG_PAYLOAD_OFF);
```

This is the **standard** pattern rather than a workaround: MCUboot pads trailer
fields to `BOOT_MAX_ALIGN` with `0xff`, and Zephyr's `stream_flash` pads
automatically. Same footgun in MCUboot #581 and TinyFPGA PR #52 ("writes < 256
bytes fail but larger writes succeed"). Writing the header *first* with a
separate valid-flag last appears in no production bootloader — payload first,
padded header last is universal.

Unreported for this driver. Similar unresolved reports exist for STM32 OSPI
(#97078, #79669) and NXP (#64702) with the identical signature.

## Why the image lives in OSPI, and how it gets there

The image is 3.32 MiB against 1.97 MiB of *total* internal flash, so embedding
it is dead on arithmetic. It goes in the octo-SPI NOR, which is memory-mapped
at `0x90000000` (FSP `BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS`, CS1) — so
loading the guest is a bare `memcpy` with no driver in the path.

Offset `0x40000` is chosen because CircuitPython's filesystem occupies
`0x0`-`0x1FFFF` and the OSPI driver rewrites its autocalibration sector at
`0x2000` on any boot where the preamble mismatches. Starting at `0x40000` also
matches upstream's own `spi_flash` sample for this board, and lets
CircuitPython and the Linux image coexist.

**SD card was evaluated and rejected.** Per the EK-RA8D1 user's manual, SW1-7
OFF *physically isolates the SDRAM from the MCU bus*, and SDHC1 requires SW1-7
OFF. Mutually exclusive with the 64 MB that makes this project viable. It is
board-level bus isolation, not a pinmux collision, so it cannot be routed
around. (The SD slot is not even on-board; it needs a PMOD shield.)

**probe-rs cannot write the OSPI** — `probe-rs chip info R7FA8D1BH` shows no
`0x90000000` region and no flash algorithm for it. Hence the in-app UART
loader. Getting probe-rs there would mean a custom target YAML plus a compiled
ARM flash-algorithm blob reimplementing a driver we already run on the target.

**Transfer took 377 s at 9,216 B/s** — 80% of line rate at 115200, so the link
is near capacity and the console baud is the limit, not USB or JTAG. A
`&uart9 { current-speed = <921600>; };` overlay should bring that to ~38 s.
For contrast, the same payload through the CircuitPython REPL would take
~9.7 hours: the REPL is 92x slower than raw UART, which is per-line interpreter
overhead, not the serial link.

## Open items

- [ ] Bump console to 921600 (one-line DT overlay) before the next image push
- [ ] Hand-written bulk copy — `memcpy` at 31 MiB/s is well below what the
      150/123 MiB/s store/load figures predict
- [ ] 64 KB ITCM + 64 KB DTCM are entirely unused; obvious homes for the
      interpreter loop and hot guest state
- [ ] Guest has no I2C/SPI/GPIO — only a UART. Anything driving real hardware
      (Blinka, motor control) needs a paravirt device layer bridging guest MMIO
      to Zephyr's host-side drivers. Native CircuitPython is the better runtime
      for hardware work; Linux is the better demo.
- [ ] Userland is BusyBox with no Python and no package manager

## Provenance

Emulator: `cnlohr/mini-rv32ima`, MIT/BSD/CC0.
Image: `cnlohr/mini-rv32ima-images`, `linux-6.1.14-rv32nommu-cnl-1`.
Everything here stays on `mikeysklar/*` forks. Nothing filed upstream.
