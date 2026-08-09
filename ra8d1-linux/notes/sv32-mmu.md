# Sv32 MMU for the RV32 guest: scoping and progress log

**Goal:** give the emulated RISC-V guest a real MMU so it runs ordinary MMU Linux
with a normal glibc/musl userspace. That makes the board look like an SBC, which
means **Adafruit Blinka installs the way it does on a Pi** instead of needing a
hand-built nommu CPython.

**Status: SCOPING. No code written yet.**

---

## 1. Why this is the right target

Blinka does not need a big distro. It needs an MMU. Concretely:

- Buildroot gates `python3` on `BR2_USE_MMU` (`package/python3/Config.in`),
  annotated `# uses fork()`. That gate is the whole story.
- Blinka's I2C path is `busio` -> `adafruit_blinka.microcontroller.generic_linux.i2c`
  -> `Adafruit_PureIO.smbus`, whose module-level imports are `ctypes`, `fcntl.ioctl`
  and `struct`. All ordinary on MMU Linux; all painful under bFLT.
- Every Blinka deployment Adafruit ships is MMU: Pi, BeagleBone, Jetson, Orange Pi.

The three no-MMU Linux builds in this orbit all lack Python for the same structural
reason, which is confirmation rather than coincidence:

| Build | Chip | MMU | Python |
|---|---|---|---|
| ESP32-S3 (jcmvbkbc) | Xtensa | no | MicroPython / Lua only |
| Fruit Jam | RP2350 Cortex-M33 | no | no (dropbear also failed) |
| RA8D1 (this) | emulated RV32 | no | CPython builds, does not fully init |

## 2. What we have today

`emulator/mini-rv32ima/mini-rv32ima.h` is **548 lines**. Encouragingly small, but
it is machine-mode only:

```
grep -cE "satp|page_table|translate|tlb|vaddr|Sv32" mini-rv32ima.h   ->  0
```

- CSRs present: `mstatus`, `mepc`, `mcause`, `mtval`, `mie`, `mip`, `mscratch`,
  `mvendorid`, `misa`, `mtvec`. Machine mode only.
- `extraflags` bits 0..1 hold the privilege level; only M and U are exercised.
- Memory access is direct, with no translation layer:
  ```c
  #define MINIRV32_STORE4( ofs, val ) *(uint32_t*)(image + ofs) = val
  #define MINIRV32_LOAD4( ofs )       *(uint32_t*)(image + ofs)
  ```
  Those macros are the clean hook point: translation goes in one place.

The guest DTB (`sixtyfourmb.dtb`, and the compiled-in `default64mbdtc.h`) declares
`riscv-minimal-nommu`, so it must be replaced with an MMU machine description too.

## 3. Prior art (searched 2026-08-07)

**No complete Sv32 mini-rv32ima exists.** Findings:

- **NCKU EE final project**, "Implement MMU for mini-rv32ima to boot xv6 or Linux"
  (<https://hackmd.io/@nckuee-finalproject/CA_Final_Project>). Closest match. Adds a
  ~200-line two-level Sv32 walk hooked into fetch/load/store via a
  `mmu_translate(image, satp, ofs, MemAccessMode_*)` call. **But**: it adds only
  the `satp` CSR (0x180) and *no* S-mode machinery (no `sstatus`, `stvec`, `sepc`,
  `scause`, `medeleg`, `mideleg`, `SRET`), no TLB, and it is unfinished — ends with
  "We cannot figure out how to modify dts file yet" and never claims a boot.
  Useful as a reference for the page-walk itself, not as a drop-in.
- **GrieferPig/esp32-s31-linux** — "Porting MMU Linux to ESP32-S31". Not applicable:
  it is a *native* port to a chip that has a real MMU, not an emulator. Confirms the
  general appetite, gives us nothing to reuse.
- **TinyEMU** (Fabrice Bellard) already implements RV32 + Sv32 + S-mode and boots
  Linux. This is the significant one — see path B.

## 4. What Sv32 actually requires

Adding this to mini-rv32ima is materially more than the page walk:

1. **S-mode privilege level** — currently only M and U are used. Needs the full
   M/S/U transitions.
2. **S-mode CSRs** — `satp` (0x180), `sstatus`, `stvec`, `sepc`, `scause`, `stval`,
   `sie`, `sip`, `sscratch`.
3. **Trap delegation** — `medeleg` / `mideleg`, so page faults and timer/software
   interrupts reach S-mode instead of bouncing to M-mode.
4. **`SRET`** plus correct `mstatus` SPP/SPIE/SIE semantics.
5. **Sv32 two-level walk** — 10/10/12 split, A/D bit updates, permission checks
   (R/W/X, U bit, `sstatus.SUM`/`MXR`), superpages at level 1.
6. **Page-fault exceptions** — 12 (instruction), 13 (load), 15 (store/AMO), with
   `stval` set to the faulting virtual address.
7. **`SFENCE.VMA`** handling.
8. **A TLB.** Not optional here. Without one, every load/store/fetch costs two extra
   memory reads for the walk. On a 480 MHz M85 driving SDRAM that is a large tax on
   an already-interpreted core.
9. **New guest DTB** describing an MMU machine, replacing `riscv-minimal-nommu`.
10. **New guest build** — MMU kernel plus a full musl/glibc userspace (ELF, not bFLT).
    This also disposes of every bFLT problem logged in `04-guest-image.md`.

Items 1-4 and 7 are the ones the NCKU attempt skipped, and they are where the real
work is.

## 5. Two paths

### Path A — add Sv32 + S-mode to mini-rv32ima

- Pro: keeps the 548-line core we already have integrated with Zephyr, the paravirt
  MMIO bridge at `0x11200000`, the telnet console, and the OSPI image loader.
- Pro: the memory macros give a single clean hook point.
- Con: items 1-4 are privileged-spec work with subtle semantics, and getting them
  subtly wrong produces exactly the kind of silent misbehaviour this project has
  already spent a day on.
- Con: nobody has finished this. We would be first, with no reference to check
  against.

### Path B — port TinyEMU's RV32 core instead

- Pro: **Sv32, S-mode, delegation and a TLB already exist and already boot Linux.**
  The correctness problem is solved by someone else.
- Pro: the layering precedent already exists in this repo — `ra8d1-arcade` splits
  `emu/` (pure C, board-agnostic) from `src/platform_zephyr.c`. Same shape applies.
- Con: TinyEMU is larger and assumes a hosted environment (malloc, file I/O,
  termios), so the port is real work.
- Con: we would re-do the Zephyr integration — MMIO bridge, console, image loading.

**Current lean: Path B**, on the grounds that the hard part of Path A is correctness
of the privileged spec, and TinyEMU has already paid that cost. Worth one focused
spike to size TinyEMU's RV32 core and its host dependencies before committing.

## 5a. TinyEMU spike — RESULT: it ports (2026-08-07)

Source: `https://bellard.org/tinyemu/tinyemu-2019-12-21.tar.gz` (MIT).

**The RV32 core cross-compiles clean for Cortex-M85 and is tiny.**

```
arm-none-eabi-gcc -c -O2 -mcpu=cortex-m85 -mthumb -Ishim \
  -DMAX_XLEN=32 -DCONFIG_RISCV_MAX_XLEN=32 -DFLEN=0 riscv_cpu.c
   text    data     bss     dec
   8392       0       0    8392
```

Symbol breakdown confirms nothing was optimised away:

| Symbol | Bytes | What |
|---|---|---|
| `riscv_cpu_interp_x32` | 4852 | main interpreter loop |
| `csr_write` / `csr_read` | 804 / 672 | CSR file incl. `satp` |
| `riscv32_read_slow` / `_write_slow` | 404 / 272 | TLB-miss paths |
| `get_phys_addr` | 374 | **Sv32 two-level page walk** |
| `raise_exception2` | 292 | trap delivery |
| `riscv_cpu_flush_tlb_write_range_ram32` | 52 | TLB management |

8.4 KB of flash for a full MMU-capable RV32 core, against 2 MB available. Size is
a non-issue.

### Portability findings

- **One build fix needed.** `cutils.h:87-95` only provides an inline `bswap_32`
  fallback under `#if defined(_WIN32)`, otherwise `#include <byteswap.h>` (a
  glibc-ism absent on bare metal). An 8-line `shim/byteswap.h` mapping to
  `__builtin_bswap{16,32,64}` fixes it. Worth upstreaming.
- **Host dependencies in `riscv_cpu.c` are trivial and all droppable:**
  26 `printf` (debug), 5 `abort`, 2 `fprintf` (register dump), 1 `assert`,
  1 `fopen("/tmp/riscemu.log")` (debug logging only).
- **Exactly one allocation.** `mallocz(sizeof(*s))` in `riscv_cpu_init` (line 1296)
  and one `free(s)` in teardown (line 1326). Replace with a static struct under
  Zephyr; no dynamic allocation on any hot path.
- **Float is fully optional.** `FLEN` defaults to 64 but everything is guarded by
  `#if FLEN > 0`. Building `-DFLEN=0` for RV32IMA drops `softfp.c` entirely.
- **XLEN is a compile-time template.** The Makefile builds the same `riscv_cpu.c`
  three times; we need only `-DMAX_XLEN=32`, so the 64/128 paths cost nothing.
- **`CONFIG_EXT_C`** (compressed instructions) is on by default in
  `riscv_cpu_priv.h:40`. Harmless for an rv32ima guest; can stay.

### What a port would actually consist of

| Piece | Lines | Action |
|---|---|---|
| `riscv_cpu.c` + `riscv_cpu_template.h` + `riscv_cpu_priv.h` + `riscv_cpu.h` | ~3527 | take as-is |
| `cutils.c/h` | 314 | take, replace `mallocz` |
| `iomem.c/h` | 412 | take, or replace with our own dispatch |
| `shim/byteswap.h` | 8 | new |
| `softfp.c` | 87 | **drop** (FLEN=0) |
| `riscv_machine.c` | 1053 | **do not take** — write our own machine layer |

We keep our own machine layer so the existing Zephyr integration survives: the
paravirt MMIO bridge at `0x11200000`, the telnet console, the OSPI image loader.
That mirrors the `ra8d1-arcade` split of `emu/` (pure C) from `platform_zephyr.c`.

**Verdict: Path B is viable and clearly cheaper than Path A.** We inherit a
correct, already-Linux-booting Sv32 + S-mode + TLB implementation for 8.4 KB and
one shim, instead of hand-writing privileged-spec semantics with no reference.

## 5b. Benchmark: TinyEMU vs mini-rv32ima (2026-08-07)

Method: same Debian container (colima, arm64 Mac), same hand-assembled RV32
instruction stream, 200M instructions each. Both emulators built with `cc -O2`
in that container so the environment is identical. Benchmark kernel is a mixed
ALU + load/store loop (`/br/bench.S`), made PC-relative so the identical binary
runs at 0x1000 under the TinyEMU harness and at 0x80000000 under mini-rv32ima.

| Configuration | MIPS |
|---|---|
| mini-rv32ima, no MMU | **350.8** |
| TinyEMU, bare mode (satp=0) | 462-466 |
| TinyEMU, **Sv32 paging active, S-mode** | **464-468** |
| TinyEMU, paging active + unmapped page table (control) | 306 |

**TinyEMU with a working MMU is ~33% faster than mini-rv32ima with no MMU.**

Paging really is active in the third row. Proof is the fourth row: repointing
`satp` at an unpopulated page table (PPN 0x10 -> 0x20) drops throughput to 306
MIPS as the CPU faults on every access. If translation were not being honoured,
that patch would change nothing.

Why the MMU costs so little: TinyEMU uses a QEMU-style softmmu TLB (256 entries,
4 KB pages, `riscv_cpu_priv.h:101`). A hit is an index, a tag compare and an add
of `mem_addend = host_ptr - guest_vaddr`. Misses call `get_phys_addr` for the
two-level walk.

**Caveat, stated plainly:** this loop touches two pages, so it is a 100% TLB-hit
workload — the best case. Real Linux with many processes and context switches
will take misses that cost a page walk each. The true tax is above zero; it is
just not a cliff. A boot-to-shell comparison on a real rv32 MMU image would be
the honest follow-up, and needs a Buildroot MMU guest we do not have yet.

Artifacts (in container `br`): `/br/bench.S`, `/br/bench_mmu.S`, `/br/tebench`
(harness), `/br/mrv` (mini-rv32ima built in-container),
`/br/tinyemu-2019-12-21/bench.c`.

## 5c. Storage decision (2026-08-07): uncompressed EROFS on virtio-blk

**Do not compress the rootfs.** The reasoning is a variable neither the earlier
scoping nor the Fruit Jam history costed:

> Decompression runs **inside the emulated guest** at ~10-20 MIPS. A block read
> from the memory-mapped OSPI is a **native memcpy on the 480 MHz M85**. Every
> compression scheme converts cheap native work into expensive emulated work.

Estimated in-guest cost to decompress a 34 MB rootfs (order-of-magnitude; the
ranking is robust, the absolute numbers are not):

| Codec | Time for 34 MB |
|---|---|
| lz4 | ~9 s |
| zstd | ~15 s |
| gzip | ~50 s |
| **xz** | **~200 s** |

And flash is not scarce: 63.75 MB usable from offset 0x40000, against a ~5 MB
kernel plus ~34 MB rootfs = ~39 MB, leaving ~24 MB spare. We would be spending
our scarcest resource (emulated cycles) to conserve our least scarce one.
**My earlier "12-14 MB with squashfs+xz" figure is withdrawn** — it optimised
the wrong axis.

### Ranking

| # | Option | Verdict |
|---|---|---|
| 1 | **virtio-blk + uncompressed EROFS + overlayfs on tmpfs** | **Winner** |
| 2 | virtio-blk + squashfs, zstd or lz4, never xz | fallback if flash tightens |
| 3 | initramfs | viable but tight and fragile; fine for bring-up only |
| 4 | cramfs XIP | correct analysis, wrong problem — skip |
| 5 | squashfs on a ramdisk | dead: pays RAM *and* CPU |

### RAM, the actual argument

initramfs unpacks into **ramfs**, which has no backing store and no swap, so it
is **pinned and unreclaimable for the life of the boot**: 34 MB gone, ~22 MB left
for all of userspace against a CPython+Blinka RSS of 15-20 MB. Worse, the boot
peak has the kernel Image, the cpio blob and the unpacked tree resident at once —
uncompressed that is ~73 MB and **does not fit in 64 MB**, so initramfs *forces*
compression and therefore forces the decompression tax. It is structural.

The block device contributes only its working set, as clean **reclaimable** page
cache: ~8 MB kernel overhead, 30+ MB of elastic cache. Under pressure the kernel
evicts and re-reads instead of invoking the OOM killer.

### Two traps to avoid (both verified in source)

1. **TinyEMU's `virtio_block_init` (`virtio.c:1117`) never sets
   `device_features`, so the guest sees a WRITABLE disk.** Add
   `s->common.device_features = 1 << 5;  /* VIRTIO_BLK_F_RO */`.
2. **`CONFIG_TMPFS_XATTR` is `default n` but overlayfs REQUIRES it** on the upper
   filesystem (trusted.*/user.* xattrs). Miss it and the overlay mount fails at
   boot with a non-obvious error.

### XIP is definitively dead on riscv32 — a *different* reason than the Fruit Jam

The Fruit Jam's bFLT `gp` failure genuinely does not apply to ELF+MMU. But DAX,
the modern XIP path, is unavailable: `arch/riscv/Kconfig` has
`select ARCH_HAS_PTE_DEVMAP if 64BIT && MMU`; `ZONE_DEVICE` depends on it;
`FS_DAX` depends on `ZONE_DEVICE || FS_DAX_LIMITED`, and `FS_DAX_LIMITED` is
promptless and s390-only. **No `-o dax` on rv32.** The only surviving route is
cramfs + `CONFIG_CRAMFS_MTD`, which is real but charges a 16 MB max file size, no
hardlinks, no timestamps, and degrades silently to ordinary paging when any of
its conditions fail. It is also not even a speed win: OSPI random-access latency
is worse than SDRAM.

### Machine-layer cost (measured, arm-none-eabi-gcc 14.3, -O2 -mcpu=cortex-m85)

- TinyEMU `virtio.c` cross-compiles clean: 13,168 B text for all devices;
  **5,166 B block-only** after `-ffunction-sections -Wl,--gc-sections`.
- The `BlockDevice` backend is ~25 lines: `read_async` returning `<= 0` means
  synchronous completion, so the read is literally
  `memcpy(buf, (uint8_t*)0x90000000 + ROOTFS_OFF + sector*512, n*512); return 0;`
- Replace the per-request `malloc` bounce buffer (`virtio.c:1084`) with a static
  SRAM buffer and bounds-check to `VIRTIO_BLK_S_IOERR`.
- A PLIC is needed for virtio interrupts — but that is already on the critical
  path for guest networking, so it is not a cost of this choice.

### Flash layout

```
0x000000  128 KiB  CIRCUITPY (leave alone)
0x040000    ~5 MB  kernel Image
0x600000   ~34 MB  EROFS rootfs (256 KiB aligned)
                   ~24 MB spare of 63.75 MB usable
```

### Guest kernel config

```
CONFIG_MMU=y  CONFIG_BLOCK=y  CONFIG_BLK_DEV=y
CONFIG_VIRTIO=y  CONFIG_VIRTIO_MMIO=y  CONFIG_VIRTIO_BLK=y
CONFIG_EROFS_FS=y
# CONFIG_EROFS_FS_ZIP is not set     # deliberate: no decompressor on the hot path
CONFIG_OVERLAY_FS=y  CONFIG_TMPFS=y  CONFIG_TMPFS_XATTR=y   # XATTR is default n!
CONFIG_DEVTMPFS=y  CONFIG_DEVTMPFS_MOUNT=y
CONFIG_BLK_DEV_INITRD=y               # tiny initramfs /init that switch_root's
```

Buildroot: `BR2_TARGET_ROOTFS_EROFS=y` + `BR2_TARGET_ROOTFS_EROFS_NONE=y`.

### Blinka does not need a writable rootfs

Its I2C path only opens `/dev/i2c-N` and issues ioctls. `/tmp`, `/run`, `/var`
and `pip install` are all served by the tmpfs overlay (not persistent across
reboot, which is acceptable).

**Ship `.pyc` only** — on a read-only root CPython cannot write `__pycache__` and
will recompile every imported module on **every boot** at emulated speed. Build
with `python -m compileall --invalidation-mode unchecked-hash` so fixed
filesystem timestamps do not invalidate the cache. This buys boot time, not just
the ~11.7 MB of duplicate sources.

## 5d. MMU guest BUILT AND BOOTED — the pivot is validated (2026-08-07)

Buildroot `qemu_riscv32_virt_defconfig` + python3/libffi/i2c-tools, built
out-of-tree at `/br/mmu`. `EXIT=0`. Booted under `qemu-system-riscv32`.

```
Linux buildroot 6.1.44 #1 SMP riscv32 GNU/Linux
Python 3.11.6

python3 -c "import ctypes, fcntl, struct; print('IMPORT-OK')"     -> IMPORT-OK
python3 -c "import ctypes; l=ctypes.CDLL('libc.so.6'); ..."       -> CDLL-OK pid=125
python3 -c "...ctypes.CFUNCTYPE(c_int,c_int)(lambda x:x*2)..."    -> CLOSURE-OK 42
```

**This is the whole thesis proven.** `ctypes` is the hard dependency of
`Adafruit_PureIO` and was structurally impossible on nommu/bFLT (CPython #81241:
`import ctypes` calls `dlopen(NULL)`, which fails in any static interpreter).
`CFUNCTYPE` working means **libffi is generating and executing trampolines on
riscv32** — the deepest part of ctypes and the most likely thing to be broken on
an uncommon architecture. udhcpc also got a lease (10.0.2.15) under QEMU user
networking.

Compare with the same board this morning: the hand-built bFLT CPython loaded,
printed its version, then jumped to an unrelocated `0x13c` and panicked the guest
kernel. The MMU path just works, because ELF + dynamic linking is what CPython is
designed for.

### Measured sizes

| Artifact | Bytes | Note |
|---|---|---|
| `rootfs.tar` | 45,045,760 | real content |
| `rootfs.ext2` | 62,914,560 | Buildroot padding — do not ship |
| `Image` | 25,923,072 | 64% zeros, gzips to 5,941,717 |
| `fw_jump.bin` | 123,080 | OpenSBI |
| finalized `target/` | 44 MB | |

`PYC_ONLY` fired at finalize exactly as predicted: `.py` 555 -> **0**,
`.pyc` 0 -> **555**, stdlib 26 MB -> **18 MB**.

### The kernel is fat because of the defconfig, not the architecture

`qemu_riscv32_virt_defconfig` enables **1100 options**, 153 of them
PCI/USB/sound/DRM/SCSI — a full QEMU virt driver set. Our machine has a UART, a
CLINT, a PLIC and virtio. A custom defconfig should reach 3-5 MB. Note kernel
compression is CHEAP (the host M85 inflates it natively before the guest runs),
unlike rootfs compression which would run inside the emulated guest — the
"don't compress" rule in §5c applies to the ROOTFS ONLY.

**Measured: the kernel's zero padding is NOT trailing.** Only 220 bytes trail;
the 16.65 MB of zeros are scattered through alignment gaps. So stripping trailing
zeros (a proposed free host-side win) does not work. Gzip is the answer.

### Flash budget: raw blobs DO NOT FIT

```
Image 25,923,072 + rootfs 45,045,760 = 70,968,832   vs 63.50 MiB usable slot
                                                     OVER BY 4.2 MiB
gz kernel 5,941,717 + rootfs 45,045,760 = 48.6 MiB   fits, 15 MiB spare
```

Compressing the kernel is **required**, not an optimisation. `rootfs.ext2` fits
alone but cannot coexist with any kernel — ship the EROFS, not the padded ext2.

### And the SDRAM constraint makes XIP-from-OSPI MANDATORY

A 45 MB rootfs copied into SDRAM leaves 19 MB; kernel + rootfs at 71 MB simply
**exceeds the 64 MB SDRAM**. The current loader's copy-everything-to-guest-RAM
design does not scale. The rootfs must be read in place from the memory-mapped
OSPI window at `0x90000000` via a paravirt/virtio block device. This turns
§5c's recommendation from "preferable" into "the only thing that works".

### Loader: three real bugs found and fixed (built, NOT flashed)

1. **The erase could not reach the top of the chip.** `flash_get_page_info()`
   returns `-EINVAL` one past the final page, so an erase ending exactly at
   64 MB fails. Fixed by reserving the last 256 KB block.
2. **`img_receive()` never validated the wire length.** A raw 32-bit value fed
   `ROUND_UP(len + IMG_PAYLOAD_OFF, 256K)`, which wraps — `len=0xFFFFFFFF`
   erases one block then loops for hours. A desynced host could destroy a
   working image.
3. **`guest_prepare()` had no RAM bound.**

Also added: a boot-time 'L' prompt to enter the loader deliberately (the old code
only reached it when `img_check()` FAILED), and a per-block erase progress
indicator (172 blocks / ~86 s at 45 MB is otherwise indistinguishable from a
hang). Build: 143,024 B flash (6.93%), 74,792 B RAM (8.15%), +620 B for the
changes. **The loader still assumes ONE image at ONE offset** — two artifacts
needs a second slot or a container header.

### Push time is the remaining friction

45 MB over UART at 921600 with 4 KB ACK flow control = **12m49s**
(8m09s wire + 1m26s erase + 3m14s writes). Ranked fixes: push over **TCP**
(the telnet bridge already proves Ethernet/sockets/threads — ~4 min, flash
programming becomes the floor); gzip the kernel; double-buffer receive against
flash write (~3m14s of the 12m49s).

### I2C zero-length write: `main.c` is exonerated

Traced end to end: `i2c_write(dev, buf, 0, addr)` -> `iic_master_read_write`
sets `addr_total = 1` -> the address byte IS sent -> NAK sets `err` -> the STOP
handler's `(0 == remain) && (false == err)` test fails -> `I2C_MASTER_EVENT_ABORTED`
-> `-EIO`. So a zero-length write genuinely puts START + address + STOP on the
wire and reports absent devices correctly.

Better suspect, in `main.c` but owned by whoever fixes `pv-io.c`:
**`pv_i2c_wlen` is a static that `pv_exec` never clears between commands.** If
`pv-io.c` omits the WLEN register write when `len == 0`, the Zephyr side reuses
the PREVIOUS command's length and sends stale bytes from the shared DATA window —
a garbage byte to every address in a scan loop. Also: `PV_CMD_I2C_PROBE` already
exists and does the right thing; routing `write_quick()` to it removes the
question entirely.

## 6. Open questions

- How fast is an MMU-emulating core on the M85? Every guest memory access gains a
  TLB lookup, and misses cost a two-level walk. Need a measurement, not a guess.
- Does 64 MB SDRAM still suffice for an MMU kernel plus glibc userspace plus page
  tables? Probably, but unmeasured.
- Does the paravirt MMIO bridge survive unchanged? It sits at `0x11200000` and is
  addressed physically, so it should, but the guest driver would now see it through
  translation.
- Can we keep the existing OSPI image path, or does a bigger rootfs force a rethink?

## 7. Progress log

| Date | Entry |
|---|---|
| 2026-08-07 | Scoped. Confirmed mini-rv32ima has zero translation today and is M-mode only. Surveyed prior art: NCKU attempt is partial and unfinished; TinyEMU already has Sv32+S-mode. Enumerated the 10 requirements above. Leaning Path B. No code written. |
| 2026-08-07 | **TinyEMU spike done — it ports.** RV32 core cross-compiles for Cortex-M85 at 8392 B text with one 8-line `byteswap.h` shim. `get_phys_addr` (Sv32 walk), TLB paths and full CSR file all present. Only one `mallocz`, no hot-path allocation, float droppable via `FLEN=0`. Verdict: Path B confirmed viable. Next: measure emulated-MMU throughput before committing, and decide the guest build (MMU kernel + musl rootfs). |
| 2026-08-07 | **Benchmarked — TinyEMU wins outright.** 464-468 MIPS with Sv32 paging active vs 350.8 MIPS for mini-rv32ima with no MMU, same container and same instruction stream. Paging verified active via an unmapped-page-table control (306 MIPS). The performance objection to Path B is answered: adopting an MMU is not a speed regression, it is a speed *improvement*. Remaining unknowns are the guest build (MMU kernel + musl/glibc rootfs) and a boot-to-shell measurement on a real rv32 MMU image. |
| 2026-08-08 | **MMU Linux on silicon.** Trimmed kernel (25.9 → 6.24 MB: `STRICT_KERNEL_RWX=n` recovered 15.2 MB of 4 MiB section padding, driver trim the rest) fits the 8 MB OSPI slot raw. Boots to a login prompt in 34.5 s at 15.37 MIPS. Python 3.11.6 + `ctypes` confirmed on the board. `pv-io.c` compiled for the first time (4 bugs: missing `MODULE_LICENSE`, SMBUS_QUICK in the functionality mask, const `quirks` initialiser, probe by dereference → `copy_from_kernel_nofault`) and found the real bridge: `bridge v1, 1 i2c bus(es), 8 gpio(s)`. New blocker: console input reaches getty but not the shell. |
| 2026-08-08 | **Blinka works on the board — the goal of this whole document.** Built a 32 MB rootfs with Blinka 9.2.0 + PlatformDetect + PureIO and the `EK_RA8D1_RVLINUX` port installed in full-detection mode; all tests autorun from `S99i2ctest` at boot so nothing needs the broken interactive console. QEMU caught two trimmed-stdlib gaps (`email`, `urllib`) before any push. On silicon, unattended: `/dev/i2c-0` present, `gpiochip504` registered, `i2cdetect -y -r 0` finds `0x14`, PlatformDetect auto-identifies `chip=RA8D1_PV board=EK_RA8D1_RVLINUX` off the bridge's adapter name, and `board.I2C()` scan returns `['0x14']`. Push reliability root-caused to app uptime: six consecutive erase-then-stall failures on a ~12 h-old app, clean pushes immediately after `probe-rs reset`; `pushimage.py` retry loop also fixed to catch socket exceptions. |
