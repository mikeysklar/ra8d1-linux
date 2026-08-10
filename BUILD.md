# RA8D1: Linux guest with CPython, from a stock board

Minimal path only. Steps that did not pan out are omitted; see
`ra8d1-linux/notes/` for the full record including the dead ends.

**What this gets you:** a riscv32 Linux guest with an MMU, glibc, and CPython 3.11
with working `ctypes`, running inside an emulator on the Cortex-M85, reachable
over telnet. That is the configuration Adafruit Blinka needs.

**Status, stated honestly:**

| Piece | Proven where |
|---|---|
| MMU guest boots, Python + `ctypes` work | QEMU, and under the TinyEMU host harness |
| TinyEMU machine layer boots Linux to userspace | host harness (Mac) |
| Emulator running on real RA8D1 silicon | yes, 91 KB flash, **15.37 MIPS** |
| Console input on real hardware | **yes, fully — interactive shell, root caused and fixed (trap 17)** |
| **`pip install` on the board, offline** | **yes — 4 CircuitPython drivers from a local wheelhouse** |
| **Interactive Blinka over serial and over telnet** | **yes — `blinkatest.py` typed at the prompt** |
| **MMU Linux booting on the board** | **yes — login prompt in 34.5 s, from OSPI flash** |
| **Python 3.11.6 + `ctypes` on the board** | **yes** |
| **`pv-io.c` → `/dev/i2c-0` on the board** | **yes — bridge v1, 1 i2c bus, 8 gpios, `gpiochip504`** |
| **`i2cdetect -y -r 0` against real silicon** | **yes — finds the device at `0x14`** |
| **Blinka on the board, full auto-detection** | **yes — `chip=RA8D1_PV board=EK_RA8D1_RVLINUX`, `board.I2C()` scan returns `['0x14']`** |
| nommu guest + telnet on real silicon | yes, working |
| **Guest has its own NIC, MAC and DHCP lease** | **yes — virtio-net bridged to eth0, `192.168.2.4` on the LAN (2026-08-09)** |
| **`ssh root@<guest>` + scp from another machine** | **yes — dropbear, ~9 s first connect, scp -O md5-verified both ways** |
| **In-app image pusher (no app swap)** | **kernel-sized: yes, proven. 54 MB: open bug, gotcha 27 — use rvlinux for big images** |

Those were first proven 2026-08-08 on real hardware, unattended: the rootfs ran
the whole test from `/etc/init.d/S99i2ctest` at boot and the results were read
from the boot log, sidestepping trap 17. Later the same day trap 17 was root
caused and fixed, and the Adafruit `blinkatest.py` was run by hand at the
prompt — over the serial console and over telnet — so the autorun script is now
a convenience rather than the only way in.

**Speed, measured on silicon:** 15.37 MIPS with Sv32 paging active, D-cache on,
self-measured from the guest's own `instret`/`time` counters. The same loop runs
at 293 MIPS under the host harness on an arm64 Mac, so **the M85 is ~19x slower
than a development machine.** Treat every Mac number as a ceiling. That figure is
also best case: it is a TLB-hit, cache-resident loop, and real code takes
page-walk and SDRAM misses it does not.

---

## Sources and patches

Everything needed to rebuild this from scratch. Nothing here lives only in a
Docker container.

### Repositories

| What | Where | Revision used |
|---|---|---|
| Zephyr (fork) | `git@github.com:mikeysklar/zephyr.git` | branch `ra8d1/glcdc-64byte-alignment`, `1f6f2c3b4d7`, v4.4.99 |
| CircuitPython (fork, carries the west workspace) | `git@github.com:mikeysklar/circuitpython.git` | branch `ra8d1/integration` |
| West workspace root | `circuitpython/ports/zephyr-cp/` | manifest at `zephyr-config/west.yml` |
| Buildroot (guest) | `https://gitlab.com/buildroot.org/buildroot.git` | `--depth 1` is fine |
| TinyEMU (emulator core) | `https://bellard.org/tinyemu/tinyemu-2019-12-21.tar.gz` | MIT |

The RA8D1 work itself (`ra8d1-linux/`, `ra8d1-tinyemu/`, `ra8d1-arcade/`) lives
at **`git@github.com:mikeysklar/ra8d1-linux.git`**. Build output, guest images
and the `cnlohr/mini-rv32ima` checkout are excluded by `.gitignore` — this file
is what regenerates them.

**Where the kernel is actually built, which the repo does not show.** `pv-io.c`
is versioned here as a standalone driver, but the shipped kernel builds it
**in-tree**: the file is copied to
`linux-6.1.44/drivers/i2c/busses/pv-io.c` in the trimmed tree (`/br/mmu-trim`
in the container) and built with
`make ARCH=riscv CROSS_COMPILE=riscv32-linux- Image`, toolchain on `PATH` from
`/br/mmu/host/bin`. There is also an out-of-tree `obj-m` skeleton at
`/br/mmu-pv/` used during bring-up; it produces a `.ko` nobody loads, since the
driver must be built in to probe before `/sbin/init`.

### Patches and modified files

| File | Why |
|---|---|
| `ra8d1-tinyemu/emu/shim/byteswap.h` | TinyEMU's `cutils.h:87` only provides the `bswap_32` fallback under `#if defined(_WIN32)`, so it will not build freestanding. 8 lines mapping to `__builtin_bswap*`. Worth sending upstream. |
| `ra8d1-tinyemu/guest/trim-setup.sh` | Extracts a pristine 6.1.44 tree for the kernel trim |
| `ra8d1-tinyemu/guest/trim-config.sh` | The whole kernel trim as `scripts/config` calls, with reasons |
| `ra8d1-tinyemu/guest/linux-6.1.44-trim.config` | The resulting config, so the 6.24 MB kernel is reproducible |
| `ra8d1-linux/guest/pv-io.c` | The paravirt I2C/GPIO driver, **with four required fixes applied** (see below) |
| `ra8d1-linux/guest/pv-io.c.orig-unfixed` | The original, kept only for reference. **Does not compile, and oopses on boot.** Do not build this one. |

Six patches to TinyEMU's vendored sources are marked in-file in
`ra8d1-tinyemu/emu/`; `notes/00-port.md` §4 lists them with rationale. The
important ones: `time`/`timeh` CSRs implemented (upstream leaves them for M-mode
firmware to trap, and we have none), `COUNTEREN_MASK` bit 1 so S-mode can be
granted TM, and an inverted `#ifdef` around `free(s)` in `riscv_cpu_end` that is
an upstream bug.

### The four `pv-io.c` fixes, already applied

Listed because the file had **never been compiled** before this work, and a
never-compiled driver accumulates these silently:

1. `MODULE_LICENSE("GPL v2")` was missing entirely — `modpost` fails the build
   outright on 6.1, before linking.
2. Probe uses `copy_from_kernel_nofault()` instead of a bare `readl()`. On
   RISC-V a load from an unassigned physical address **faults**; it does not read
   zero. As a `device_initcall` that was an oops during boot.
3. ~~`I2C_AQ_NO_ZERO_LEN` added to `pv_i2c_quirks`~~ — **superseded 2026-08-08,
   see below.**
4. ~~`I2C_FUNC_SMBUS_QUICK` masked out of the functionality bits~~ —
   **superseded with it.**

Fixes 3 and 4 were the first working answer to `i2c.scan()` reporting all 112
addresses as present, but they were a workaround: the quirk made the i2c core
*reject* every zero-length write a scan issues, logging
`adapter quirk: no zero length` once per address — 112 lines per scan — and the
correct result came back only because the core then fell back to a one-byte
read. No probe reached the wire.

**Both are now reverted.** `pv_i2c_xfer()` dispatches a zero-length write to
`PV_CMD_I2C_PROBE`, which the host bridge always implemented and nothing ever
called, and `SMBUS_QUICK` is advertised again. Verified on hardware: the 112 log
lines are gone and the scan still returns exactly `['0x14']` with no false
positives, which is the failure mode trap 10 warns about. `PV_I2C_WLEN` is still
written on the probe path — the host keeps it in a register the guest owns and
no command clears, so skipping it would reuse the previous transfer's length.

### Upstream driver problems, now fixed in the fork (2026-08-09)

Both issues below are FIXED on the fork's `ra8d1/glcdc-64byte-alignment`
branch; the issue numbers carry the investigation trail.

- **mikeysklar/zephyr#10** — `eth_renesas_ra.c` had no `.set_config`, so
  promiscuous mode was unreachable. Fixed in `8ca5eb88530` (capability +
  handler, durable across re-link) and hardened in `d6346d5dd3a` (PRM is
  only written with the receiver disabled — gotcha 26). Upstream still has
  neither. The fork also carries upstream PR #115734's TX/RX robustness
  fixes as `1b5f1c41b1d`.
- **mikeysklar/zephyr#11** — `PAGE_SIZE_BYTE 64` turned out to be the
  *hardware's* combination-write ceiling, not a sloppy constant (the issue
  thread has the correction); bigger pages need the FSP DMAC path. The real
  costs were elsewhere and are fixed in `d6346d5dd3a` + `122c70fe591`:
  tick-quantized status polling (write rate 71 → 146 KB/s), a 1.8x erase
  timeout margin (doubled), and — the big one — the WIP-assertion race of
  gotcha 25, which is the leading explanation for the historical
  intermittent verify miss.

## 0. Hardware

- Renesas EK-RA8D1
- **SW1-5 ON** for Ethernet. Renesas' table implies this conflicts with SDRAM.
  It does not: SDRAM still reports 64 MB and Linux boots. Verified.
- Ethernet cable. A Mac with Internet Sharing works as the DHCP server.

## 1. Host tools

```sh
brew install probe-rs        # NEVER JLinkExe: it force-updates the on-board
                             # probe firmware and bricks it
brew install --cask gcc-arm-embedded
```

Buildroot cannot run on macOS (case-insensitive filesystem). Use a Linux
container:

```sh
colima start --cpu 8 --memory 12 --disk 100
docker run -d --name br -v br:/br debian:12 sleep infinity
docker exec br apt-get update
docker exec br apt-get install -y build-essential git bc bison flex libssl-dev \
    libncurses-dev python3 rsync wget cpio unzip file
```

## 2. Build the guest (Linux + CPython)

Inside the container. This is a stock Buildroot defconfig plus four options.

```sh
git clone --depth 1 https://gitlab.com/buildroot.org/buildroot.git /br/buildroot
cd /br/buildroot
make O=/br/mmu qemu_riscv32_virt_defconfig
```

Append to `/br/mmu/.config`:

```
BR2_PACKAGE_PYTHON3=y
BR2_PACKAGE_PYTHON3_SSL=y
BR2_PACKAGE_PYTHON3_ZLIB=y
BR2_PACKAGE_PYTHON3_PYEXPAT=y      # REQUIRED: pip cannot bootstrap without it
BR2_PACKAGE_LIBFFI=y               # REQUIRED: _ctypes, which Blinka depends on
BR2_PACKAGE_I2C_TOOLS=y
BR2_TARGET_ROOTFS_EXT2=y
```

Kernel fragment for I2C/GPIO (`/br/mmu-kernel-fragment`, referenced by
`BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES`):

```
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=y
CONFIG_GPIOLIB=y
CONFIG_GPIO_CDEV=y
CONFIG_GPIO_SYSFS=y    # REQUIRED: Blinka's pin backend reads /sys/class/gpio.
                       # CONFIG_GPIO_CDEV alone does nothing for it.
```

```sh
make O=/br/mmu olddefconfig
make O=/br/mmu                     # ~45 min: builds a glibc toolchain from source
```

Output in `/br/mmu/images/`: `Image` (kernel), `rootfs.ext2`, `rootfs.tar`.

### Shrink the kernel before you go near flash

A stock `qemu_riscv32_virt_defconfig` kernel is **25.9 MB and reserves 26 MB of
the guest's 64 MB RAM**. Two changes take it to 6.5 MB and give the guest 19 MB
back. Do them in this order, because the order is the whole trick.

**1. Turn off `CONFIG_STRICT_KERNEL_RWX`. This is 57% of the image and costs no
features.** On rv32 it aligns `.text`, `.init.text`, `.rodata` and `.data` to
Sv32 **4 MiB** megapage boundaries (`arch/riscv/include/asm/set_memory.h` picks
2 MiB for rv64 and 4 MiB for rv32), applied at five boundaries in
`arch/riscv/kernel/vmlinux.lds.S`. On a kernel with 7.6 MB of `.text` that lands
the sections at 0, 8, 16 and 24 MB and the Image is the whole span:

```
vmlinux sections total   11,067,213
Image on disk            25,923,072
padding                  14,855,859   (57%)
```

The padding sits **inside** the reserved `_start.._end` span, so it costs real
SDRAM, not just file bytes. Verified that removing it does NOT lose Sv32
megapage mappings: on rv32 the kernel is covered by the linear-map loop, and
with RWX off `pgprot_from_va()` returns uniform `PAGE_KERNEL_EXEC`, so
`best_map_size()` still returns `PMD_SIZE`. `mark_rodata_ro()` is not called at
all, so there is no post-boot page-table splitting either — strictly fewer PTEs.

Cost is kernel hardening (read-only text, non-executable rodata), which an
emulated bring-up guest is not relying on.

The padding fix alone is **not enough**: measured, it lands at 10,727,424 bytes,
still 2.3 MB over an 8 MB slot. Both steps are needed.

**Do the padding fix FIRST, or you cannot measure anything else.** While the
4 MiB alignment is in place, whatever you cut just becomes more padding and the
Image barely moves. Measured, `-Os` alone on the stock config:

```
Image    25,923,584 -> 25,906,176     -17,408  (-0.07%)   looks worthless
.text     7,579,850 ->  6,266,754  -1,313,096  (-17.3%)   actually saved 1.25 MB
```

Anyone measuring with `ls -l Image` will conclude their work did nothing. Until
`STRICT_KERNEL_RWX=n` is set, measure with `size -A vmlinux` per-section instead.

(`-Os` at 17.3% off `.text` is well above the 5-10% usually quoted for x86,
which is consistent with RVC being on: compressed encodings make the smaller
instruction selection pay twice. It is a further lever if you need one; the
driver trim below was enough here.)

**2. Then cut drivers the machine cannot reach.** 1178 -> 724 symbols: network
vendors, DRM, USB, PCI, WLAN, netfilter/IPv6, NFS/9p, RTC, MMC/SCSI/ATA, EFI,
KVM, input, hwmon, thermal, watchdog, regulator, IIO, media, PWM, SPI, VT.

**Both steps are scripted — do not redo them by hand:**

```sh
sh ra8d1-tinyemu/guest/trim-setup.sh     # pristine 6.1.44 into /br/mmu-trim
sh ra8d1-tinyemu/guest/trim-config.sh    # the padding fix + every driver cut, with reasons
```

`ra8d1-tinyemu/guest/linux-6.1.44-trim.config` is the resulting config if you
would rather diff than re-run. The rule for what was safe to cut is the
emulator's own devicetree: 8250 at 0x10000000, virtio-mmio at 0x10001000, CLINT
at 0x11000000, syscon at 0x11100000, paravirt bridge at 0x11200000, PLIC at
0x40100000, RAM at 0x80000000. Nothing else exists on this machine.

Keep `pv-io.c` built in (`obj-y` in `drivers/i2c/busses/Makefile`, no Kconfig
symbol) rather than as a module — a module built against a different kernel
config will not load, and nothing on this rootfs auto-loads modules anyway
(busybox modprobe, devtmpfs only, no mdev/eudev).

Result, measured:

| | before | after |
|---|---|---|
| `Image` | 25,923,072 | **6,543,776** (3.96x) |
| `Memory: available` | 39,204K | **58,216K** |
| `reserved` | 26,332K | **7,320K** |

**Compression is then unnecessary.** 6.5 MB fits an 8 MB slot raw with 1.8 MB
spare. riscv has no self-extracting kernel head anyway (only EFI zboot, which
needs UEFI), so `Image.gz` would have to be inflated by the loader. If you ever
do need it, use **lz4** (812 B of decompressor, time-neutral against a straight
copy because it is bounded by SDRAM write bandwidth), not gzip or xz.

If you need more guest RAM later, three more config lines are worth ~1.5-1.8 MB
(measured): `CC_OPTIMIZE_FOR_SIZE=y` (~0.8-1.0 MB, `-Os` takes 17% off `.text`
and every byte of `.text` is a byte of reserved SDRAM), `LOG_BUF_SHIFT` 17 -> 15
(~0.4 MB — the static printk ringbuffer is 528 KiB resident at 17), and
`MEMCG=n` (64 KiB). Note the debug options are NOT worth chasing: the entire
`DEBUG_*` pile is only ~76 KiB of `.text`.

Everything else is floor. After the padding fix the reservation beyond the
kernel span is **700 KiB and irreducible** — 576 KiB of it is the `struct page`
array at 36 B/page, which scales with RAM size, not kernel size. `FLATMEM` is
already selected and is **2x cheaper than SPARSEMEM** here (riscv sets
`SECTION_SIZE_BITS 27`, so sparsemem populates a whole 128 MB section
regardless). Page tables cost zero — `best_map_size()` returns `PMD_SIZE` for
the whole linear map.

**Do not switch to an initramfs to simplify booting.** It pins the whole rootfs
in tmpfs — ~44 MB of a 64 MB machine, unreclaimable. Block-backed root over
virtio-blk keeps the rootfs as clean, reclaimable page cache.

### Size the rootfs to the slot

Buildroot emits `rootfs.ext2` at a **fixed** `BR2_TARGET_ROOTFS_EXT2_SIZE`,
not at content size. The default 60 MiB does not fit a 55.75 MB slot even though
the content is ~44 MB. Either set that option, or shrink after the fact:

```sh
e2fsck -f rootfs.ext2 && resize2fs -M rootfs.ext2
```

Verify before going further:

```sh
/br/mmu/host/bin/qemu-system-riscv32 -M virt -m 512M -bios fw_jump.elf \
  -kernel Image -append "rootwait root=/dev/vda console=ttyS0" \
  -drive file=rootfs.ext2,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 -nographic
```

At the shell, the check that matters:

```sh
python3 -c "import ctypes, fcntl, struct; print('OK')"
```

`ctypes` is the hard dependency of `Adafruit_PureIO`. If it imports, Blinka is
viable on this guest.

## 3. Build the emulator app

```sh
cd ra8d1-tinyemu
west build -b ek_ra8d1
```

The TinyEMU core in `emu/` is vendored from the tarball listed under Sources,
with the `shim/byteswap.h` and six in-file patches already applied. Build it
`-DMAX_XLEN=32 -DCONFIG_RISCV_MAX_XLEN=32`; `TINYEMU_FLEN` defaults to 64 and
**must stay non-zero** — the Buildroot userspace is hard-float (`ilp32d`), and
at `FLEN=0` the kernel boots fine and then init dies with `SIGILL` on the first
FP instruction in glibc, because Linux on RISC-V does not emulate absent FP.

Test on the host first. It is seconds per iteration there and minutes on the
board:

```sh
cd host && make && ./tinyemu-host /br/mmu/images/Image
```

Note the host harness has no I2C, so `pv-io.c` will find the bridge, report
`0 i2c bus(es)`, and return without registering an adapter. That path can only
be exercised on the board. Also note the emulator returns 0 for reads outside
any registered range where real hardware faults, so it is **more forgiving than
silicon** — see trap 15.

## 4. Flash

```sh
sh rvlinux/flash.sh          # strips RA option-setting sections, then probe-rs
```

The script strips `.option_setting*` into a scratch ELF first because probe-rs
cannot map those regions.

## 5. Push guest images

The board runs a TCP image loader on port 5555 with two flash slots:

| Slot | Offset | Size | Magic |
|---|---|---|---|
| kernel | 0x040000 | 8 MB | `RA8LINUX` |
| rootfs | 0x840000 | 55.5 MB | `RA8ROOTF` |

```sh
python3 rvlinux/tools/pushimage.py Image.gz     --slot kernel --tcp <board-ip>
python3 rvlinux/tools/pushimage.py rootfs.ext2  --slot rootfs --tcp <board-ip>
```

`--dry-run` validates the file and prints the CRC without touching the board.
UART fallback (`-p /dev/cu.usbmodem*`) exists for recovery.

**Budget ~65 KB/s end to end, not what the tool models.** Measured: 6.5 MB
kernel in 1m42s, 36 MB rootfs in 9m32s. The tool's own estimate is optimistic by
~2.4x, mostly on erase. A full 55 MB slot is ~14 minutes. Socket throughput is
80-82 KB/s, so the gap is board-side flash programming, not the network.

### A rootfs push can fail verification, intermittently, and it gets worse with size

Not fully root-caused as of this writing. The board writes the full payload,
reads it back, and the CRC sometimes disagrees — a real fault, not a transport
timeout. Two live hypotheses: an OSPI write/read timing margin, or an
**incomplete erase** (NOR flash erase only sets bits to 1; program only clears
them to 0, so a marginal erase produces stable-but-wrong data indistinguishable
from a bad write without checking the erase itself). `pushimage.py --retries 3`
(the default) retries automatically — a failed push has already erased the slot,
so retrying costs nothing further.

**A first failure-rate estimate here was contaminated by two now-fixed loader
bugs** (a silently-broken `SO_RCVTIMEO` that could wedge the accept loop after
any failed push, and an unbounded semaphore wait that turned a slow write into
a spurious failure) and overstated the true rate roughly 2x — treat any measurement
taken before both fixes as void, since a single transient could manufacture a
run of consecutive "failures." Re-scored on clean data: **~25% per attempt at
24 MB**, giving `--retries 3` a ~95%+ landing rate even at ~38 MB. There is no
longer a strong case for preferring a smaller rootfs on retry-rate grounds alone
— it remains a nice-to-have, not the lever it first looked like.

**What does not change: the fault is real.** A 24 MB push produced a full
`DIAG` block — payload written, read back **stably** across two passes, and
wrong. That is not explained by either fixed loader bug. The blank-check and
block-diff instrumentation (below) are armed to catch it decisively next time.

### What to expect on the board

```
kernel: 6543776 B, crc ok in 107 ms
rootfs: 36 MB, read in place from OSPI
[    6.560388] VFS: Mounted root (ext2 filesystem) readonly on device 254:0.
[    6.579862] Run /sbin/init as init process
buildroot login: root
# python3 -c "import ctypes,sys;print(sys.version.split()[0],ctypes.CDLL(None))"
3.11.6 <CDLL 'None', handle 9471ead0 at 0x9409edb0>
# free
Mem:  58400 total   50580 available
```

| | |
|---|---|
| reset -> login prompt | 34.5 s |
| of which kernel boot | 5.7 s |
| of which waiting for a nonexistent `eth0` | **~15 s** |
| guest RAM available with Python loaded | 50,580K of 58,400K |

Two throughput figures that measure different things and must not be compared:
**61.2 MB/s** is the OSPI window itself (the app CRCs 6.5 MB of kernel out of
memory-mapped flash in 107 ms). **3.4 MB/s** is what the guest gets through
`/dev/vda`, because every sector crosses the virtio ring and the interpreter
(4 MiB in 1.18 s, 0.99 s of it `sys`). The gap is emulation, not storage.

Delete `/etc/network/if-pre-up.d/wait_iface` if the machine has no NIC. It is
43% of the boot.

## 6. Blinka

PlatformDetect does not know this board. Fast path, no upstream edits:

```sh
python3 ra8d1-linux/blinka/install.py --target /usr/lib/python3.11/site-packages --mode fast --apply
export BLINKA_FORCECHIP=RA8D1_PV
export BLINKA_FORCEBOARD=GENERIC_LINUX_PC
python3 -c "import board; print(board.I2C())"
```

Install Blinka with `--no-deps`, and pin 9.2.0:

```sh
pip install --no-deps adafruit-blinka==9.2.0 Adafruit-PlatformDetect Adafruit-PureIO \
    adafruit-circuitpython-busdevice adafruit-circuitpython-typing
```

`--no-deps` is needed because 9.2.0 requires `sysv_ipc`, a C extension with no
riscv32 wheels. Blinka 9.2.0 runs fine with it entirely absent; nothing on the
I2C path touches it. Do not let pip backtrack to 8.43.0 to satisfy that
dependency: 8.43.0 predates `board_imports.json` and `install.py` will abort.

---

## Traps that cost real time

Short list. Each of these cost hours.

1. **Buildroot cannot reconfigure a toolchain in place.** Changing
   `BR2_TOOLCHAIN_*` without a full `make clean` produces a silently
   inconsistent tree. Symptom on nommu was `waitpid: ENOSYS` and busybox init
   spinning at 100% CPU with zero output. `output/target/` is also cumulative,
   so removing a package does not remove its files. **After any config change,
   verify the artifact actually changed** (compare image byte sizes) before
   believing a test result.
2. **Match the float ABI.** A soft-float kernel boots fine on an integer-only
   core, then userspace dies with `SIGILL` on the first FP instruction, because
   Linux on RISC-V does not emulate absent FP hardware. Check with
   `readelf -h`: Flags `0x4` means double-float. Also make sure `riscv,isa` in
   the devicetree matches, or FP state is silently corrupted across context
   switches.
3. **`PAGE_SIZE_BYTE` in `flash_renesas_ra_ospi_b.h` is 64.** The S28HL512T
   page buffer is 256 or 512. Costs 4x on every flash write.
   (mikeysklar/zephyr#11)
4. **The RA Ethernet driver has no `.set_config`**, so promiscuous mode is
   unreachable and `CONFIG_NET_ETHERNET_BRIDGE` cannot bind the interface.
   (mikeysklar/zephyr#10)
5. **If you use the nommu path instead** (cnlohr/mini-rv32ima), raise
   `MAX_PAGE_ORDER` from 10 to 13 in `include/linux/mmzone.h`. His README says
   so; Buildroot's patch set does not do it. Without it, anything larger than
   4 MB fails to exec. Not needed on the MMU path.
6. **`ctypes` is the whole ballgame for Blinka.** It works on MMU + glibc. It
   cannot work on nommu/bFLT, because `import ctypes` calls `dlopen(NULL)`
   (CPython #81241). This is why the MMU path exists.
7. **An out-of-tree kernel module needs `MODULE_LICENSE("GPL v2")`.** On 6.1
   `modpost` fails the build outright — it never reaches linking:
   `ERROR: modpost: missing MODULE_LICENSE() in pv-io.o`. `guest/pv-io.c` was
   missing it entirely, unnoticed because the file had never been compiled.
   Compile every driver early even with nothing to run it on; a file that has
   never seen a compiler accumulates this class of defect silently.
8. **On RISC-V, a load from an unassigned physical address faults. It does not
   read as zero.** `pv-io.c` assumed a guest without the paravirt bridge would
   read back zero and boot anyway. It does not: `ioremap()` succeeds and the
   first `readl()` raises a load access fault. As a `device_initcall()` that is
   an oops during boot, not a failed `insmod`. Probe with
   `copy_from_kernel_nofault()`, which plants an exception-table fixup that
   riscv's `do_trap_error()` honours.
9. **`i2c_adapter.quirks` is `const`** (`include/linux/i2c.h:747`), so quirk
   flags must go in the static initialiser, not be OR'd in at probe time.
10. **A zero-length-write quirk alone does not fix `i2c.scan()`.** Blinka falls
    back from `write_quick()` to `read_byte()` on OSError, so the host bridge
    must genuinely return failure for an address that did not ACK. Measured:
    with the quirk set but a bridge that acks everything, `scan()` still
    reports all 112 addresses. Check the host side before trusting any scan.
11. **A nommu guest image and an MMU guest image are both valid rv32 Linux
    `Image`s** with the same magic, so a magic check cannot tell them apart.
    The discriminator is `text_offset` in the Image header: `0` for a
    `CONFIG_RISCV_M_MODE` (nommu) build, `0x400000` for rv32 otherwise. Loading
    the wrong one produces a silent hang with no output.
12. **Trim the kernel in the right order.** Cutting all 468 driver symbols
    *without* first turning off `CONFIG_STRICT_KERNEL_RWX` removes ~4 MB of
    content and leaves a ~21 MB Image — still 2.5x over an 8 MB slot, and it
    looks like the trim failed. The padding fix is 57%; the driver cuts are
    39% of what remains. This is rv32-specific: Sv32 megapages are 4 MiB where
    Sv39 uses 2 MiB, so it bites twice as hard here as on rv64.
13. **`CONFIG_I2C_CHARDEV=y` alone does not create `/dev/i2c-0`.** The node
    appears only when an adapter registers. On a kernel with no I2C adapter its
    absence is correct, not a broken build — do not chase it.
14. **The two Zephyr apps are not interchangeable, and mixing them gives you a
    board that looks dead but is fine.** `rvlinux` runs a nommu guest (M-mode
    kernel, `text_offset=0`); the TinyEMU app runs an MMU guest (S-mode,
    `text_offset=0x400000`). Flashing one while the kernel slot holds the
    other's image gives a console that explains itself only if you are reading
    at the right baud. Swap the app and the kernel together. Keep a copy of
    the working ELF before flashing anything — recovery is then a 20 second
    reflash instead of a rebuild.

    (Historical note: this trap used to be worse because only rvlinux had
    telnet and the TCP pusher, so the wrong app also meant "no telnet, no
    ping, no way to push". As of 2026-08-08/09 the TinyEMU app has both — the
    telnet console and an RA8LDR pusher that stops the guest, pushes, and
    reboots — so the apps differ mainly in which guest they boot.)
15. **The emulator is more forgiving than the hardware, so test drivers against
    hardware semantics.** TinyEMU's `target_read_slow()` returns 0 for reads
    outside any registered range. Real RISC-V raises a load access fault. A
    driver that probes a missing device passes under the emulator and oopses on
    silicon — which is exactly what `guest/pv-io.c` did. Probe with
    `copy_from_kernel_nofault()` rather than trusting a clean emulator run.
16. **Verify the artifact changed before believing any result.** Buildroot's
    `output/target/` is cumulative and its toolchain cannot be reconfigured in
    place, so a config change can silently produce a byte-identical image.
    Compare sizes. Several experiments in this project looked like meaningful
    results and were no-ops.
17. **FIXED 2026-08-08: console input died past the login prompt. The cause was
    polled UART receive, not the shell, not the baud rate, not the probe.**
    The symptom: typing `root` at `buildroot login:` worked and gave a `#`
    prompt, then every command produced nothing at all, not even an echo, and
    the console never recovered without a reset.

    The cause is arithmetic. The guest looks at the UART exactly once per
    emulator slice — `RV_SLICE_INSNS` is 10000 instructions, ~667 us at the
    measured 15.37 MIPS — and Zephyr's `uart_poll_in()` holds a single byte
    between looks. At 921600 a byte arrives every ~11 us, so roughly 60 arrive
    per slice and one survives. `root` is 4 characters and fits; a real command
    line does not. **Losing the trailing newline leaves the guest shell blocked
    in `read()` forever**, which is why the failure presented as dead hardware
    rather than as corrupted input.

    The fix is `CONFIG_UART_INTERRUPT_DRIVEN=y` plus an ISR that drains the
    FIFO into a ring (`CONFIG_RVT_UART_RX_RING`, default 1024) which
    `plat_getc()` then drains. It costs **452 bytes of flash and ~1 KB of RAM**.
    After it, 800-character lines pasted at wire speed survive, interactive
    `ash` works with line editing and history, and `blinkatest.py` runs typed
    by hand.

    Three plausible theories were wrong first, so do not re-run them:

    | theory | how it died |
    |---|---|
    | busybox ash raw-mode line editing | a canonical-mode `read` loop died too |
    | J-Link OB VCOM wedging | reopening the port did not revive it |
    | 921600 too fast for the OB bridge | **460800 failed at the identical 40-char line** |

    That last one is the useful control: halving the baud doubles the per-byte
    time but does nothing about a 667 us gap between reads, so an unchanged
    threshold is what proves the wire speed was never the variable.

    Diagnostic note: the emulator's own 8250 model has a 64-byte receive ring
    and counts overruns in `dropped_rx`, which **nothing ever reads or prints**.
    An unreported drop counter is a large part of why this took a day. The
    Zephyr-side ring exposes `plat_console_rx_dropped()` for that reason.

    The workaround this trap used to recommend — bake tests into the rootfs and
    read the boot log — is still the right move for unattended runs. `debugfs`
    injects a script into an ext2 image without a loop mount (the container has
    no `CAP_SYS_ADMIN`):
    ```sh
    printf "cd /etc/init.d\nwrite /path/S99mytest S99mytest\n" > cmds
    debugfs -w -f cmds rootfs.ext2
    debugfs -w -R "sif /etc/init.d/S99mytest mode 0100755" rootfs.ext2
    ```
18. **Do not overwrite a known-good flash slot to iterate.** The rootfs write
    is unreliable (trap above), so replacing a working image to add one test
    file can cost you the working image with no way back — it did here, four
    consecutive failed pushes after one clean one. Keep the last-known-good
    artifact, and re-push it before assuming you can get back to where you were.
19. **Reset the board before pushing; the pusher degrades with app uptime.**
    After the app had been up ~12 h (guest running, telnet in use), every push
    failed the same way: erase completes and acks, then the board never drains
    the payload — `BrokenPipeError` or a send stall at byte zero. After a
    `probe-rs reset`, the same images pushed clean on the first or second
    attempt. Six consecutive failures on the stale app, zero after reset.
    Related tool bug, now fixed: `pushimage.py`'s retry loop caught only the
    board's explicit error lines, not socket exceptions, so these intermittent
    faults escaped the loop and a 3-attempt push died on attempt 1.
20. **Buildroot's trimmed CPython stdlib breaks Blinka at import.** The
    `.pyc`-only guest stdlib omits `email` and `urllib`, and
    `board.py → importlib.metadata → pathlib` needs both, so `import board`
    dies with `ModuleNotFoundError` after everything else works. Copy those two
    packages from `output/target/usr/lib/python3.11/` into the rootfs. Found in
    QEMU in minutes; on hardware it would have cost a 10-minute push per
    missing module. QEMU-validate the full import chain before pushing — and
    `BLINKA_FORCECHIP=RA8D1_PV BLINKA_FORCEBOARD=EK_RA8D1_RVLINUX` exercises
    the board files under QEMU where the bridge does not exist.
21. **`probe-rs reset` re-enumerates the USB CDC console, so a serial fd held
    across a reset goes write-only.** The device node is recreated — compare
    its mtime against when the process opened it — and the stale fd keeps
    transmitting while receiving nothing. This reads exactly like "the guest
    died during boot" and is not. Any tool that resets the board must close and
    reopen the port around the reset. Note the console and the debug probe are
    two interfaces on **one** composite USB device (SEGGER J-Link, serial
    `001086859839`): `probe-rs` uses the debug interface, `/dev/cu.usbmodem*`
    is the VCOM. That also means the console is a real UART, not RTT.
22. **Blinka is slow to start because of imports, and the only cheap fix is to
    stop re-paying them.** Measured on silicon over telnet:

    | | seconds |
    |---|---:|
    | `python3 -c pass` (bare interpreter) | 7.34 |
    | same with `-S` (no `site`) | 5.57 |
    | `python3 -c 'import board'` | 30.35 |
    | same, with `BLINKA_FORCECHIP`/`FORCEBOARD` set | 29.63 |
    | **second `import board` in the same process** | **0.0007** |

    `import board` costs 21.4 s of that, and `-X importtime` attributes it to
    `json` (6.3 s), `re` (5.0 s), `adafruit_platformdetect` (4.9 s), `typing`
    (3.3 s), `pathlib` (3.3 s) and `urllib.parse` (2.1 s) — pulled in largely
    through `importlib.metadata`, not by anything I2C.

    Two conclusions worth not re-deriving. **Forcing the chip and board saves
    0.72 s, not the bulk**: it short-circuits detection *logic* while
    `board.py` still imports the whole PlatformDetect package. And a warm
    import is ~30,000x faster than a cold one, so the entire cost is
    per-process. **Keep one interpreter alive** (`python3 -i script.py`, or a
    REPL you import into once) instead of trimming Blinka's import graph.
23. **pip on the target needs three stdlib packages Buildroot trims, and pip
    hides which one is missing.** `http` (vendored urllib3), `xmlrpc` (vendored
    distlib) and `distutils`. The `xmlrpc` failure is the expensive one to
    diagnose: pip 26 eagerly imports
    `pip._internal.operations.install.wheel`, and on failure records it in
    `_MISSING_MODULES` and re-raises from an audit hook later, so the reported
    error is `No module named 'pip._internal.operations.install.wheel'` and the
    real cause appears nowhere. Get the true traceback with
    `python3 -c "import pip._internal.operations.install.wheel"`. Cheapest fix
    is to sync every entry `output/target/usr/lib/python3.11/` has that the
    rootfs lacks, rather than chasing one module per cycle — this cost four
    QEMU rounds, and would have cost four 20-minute push cycles on hardware.

    pip and setuptools themselves need **no Buildroot rebuild**: they are pure
    Python, so unpacking their wheels into `site-packages` works. And
    `ensurepip` *does* ship its bundled wheels despite `--without-ensurepip` in
    `python3.mk`, so `python3 -m venv` works — verify against the target, not
    the makefile.
24. **A wheelhouse built on a Mac hides `sysv_ipc`.** `adafruit-blinka` requires
    it only under `sys_platform == "linux"`, so `pip download` on darwin never
    asks for it and every wheel passes a pure-Python check. On the guest the
    marker flips true and installs fail with `No matching distribution found for
    sysv_ipc` — no riscv32 wheel has ever been published. Install drivers with
    `--no-deps`, naming the CircuitPython dependencies explicitly; Blinka is
    already in the image. A clean wheelhouse build is not evidence that
    on-target resolution works.
25. **Polling the Semper's WIP bit too soon after a write corrupts every
    block, and the stock driver's slow poll may be the old intermittent
    fault.** WIP asserts when the flash die *starts* the program, not when
    the OSPI controller queues it. A poll that lands in the gap reads WIP=0,
    the driver reports done, the caller programs the next 64 bytes on top of
    an in-flight burst — a prohibited second program to live 16-byte ECC
    units. Measured at the extremes: a 10 µs poll with no floor corrupted
    **864 of 864 blocks** of a 54 MB push; the stock tick-rounded 100 µs
    sleep has the same race with a ~100x smaller window, which is a plausible
    mechanism for the historical ~25%-per-attempt single-block verify miss.
    Fix in the fork (`122c70fe591`): a 150 µs busy floor before the first
    status read — below any real completion, above any issue latency. The
    same commit series doubles write throughput (71 → 146 KB/s measured) by
    busy-polling programs instead of paying the tick quantum, and doubles
    the 1.8x erase-timeout margin. Note `PAGE_SIZE_BYTE 64` is the
    *hardware's* combination-write ceiling (`OSPI_B_COMBINATION_FUNCTION_64BYTE`
    is the largest enum value) — bigger pages need the FSP's DMAC path,
    which the Zephyr driver does not wire up. Fork issue #11 has the trail.
26. **Never write `ECMR.PRM` (or any ETHERC config bit) with the receiver
    enabled.** The FSP only touches PRM inside link configuration, before
    RE/TE go on. A live 1→0 flip left the MAC receiving at a crawl — too
    slow to make progress, too alive for any timeout to name it — for the
    rest of the session. The fork's `set_config` (`d6346d5dd3a`) sequences
    RE off → PRM → RE back; the gap is microseconds. If a promiscuous
    toggle is followed by network behavior that makes no sense, this rule
    was violated somewhere.
27. **OPEN: the in-app pusher cannot yet complete a 54 MB push; kernel-sized
    pushes are proven.** Five instrumented attempts died at 6.8 MB, 6.0 MB,
    160 KB (explained: the gotcha-26 violation), and 1,048,576 bytes exactly,
    the last with promiscuous mode off and no guest running — which refutes
    promiscuous clone pressure as the sole mechanism. The universal
    signature: the board consumes every byte the host sent, then both ends
    sit — the host blocked in `sendall()` until its timeout, the board's
    30 s no-data poll never firing, no STALLED print. The leading unexplored
    suspect is Zephyr's TCP receive-window update path (a window that closes
    under flash-paced draining and never reopens; macOS persist probes would
    trickle just enough to reset the board's poll). `ra8d1-tinyemu/notes/pusher.md`
    carries the full evidence table. Until closed: push big images via the
    rvlinux app (54 MB in ~14.5 min, reliable), and kernels via either.
