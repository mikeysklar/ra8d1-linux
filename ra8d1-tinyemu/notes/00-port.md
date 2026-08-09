# 00 — Porting TinyEMU's RV32 core to Zephyr on the EK-RA8D1

**Status (2026-08-07): builds for `renesas_ek_ra8d1`, and boots the real
Buildroot rv32 Sv32 guest — Linux 6.1.44, ext2 rootfs on virtio-blk, login
prompt, Python 3.11.6 with working ctypes — against the machine layer in this
tree — and **Linux runs on the board**. Flashed 2026-08-07: rv32 Sv32 Linux
6.1.44 boots from flash to a `buildroot login:` prompt in **34.5 s from reset**,
and `python3 -c "import ctypes"` works there. The self-test measures
**15.37 MIPS with Sv32 paging active** and console input works. Full account in
§8a and §8b.**

That last part is the project's actual goal reached in emulation: Blinka needs
CPython and ctypes, and both work. Transcript in §8.

Booting it found two real bugs in the machine layer and one configuration
mismatch, all fixed; see §4 and §6. It is worth saying plainly that
none of them would have been found by inspection.

Read `ra8d1-linux/notes/sv32-mmu.md` first. It is the scoping document that
chose TinyEMU over adding Sv32 to mini-rv32ima, and it has the benchmark
(464–468 MIPS with paging active vs 350 MIPS for mini-rv32ima with none).
This note is the record of the port that decision produced.

---

## 1. What was built

```
ra8d1-tinyemu/
  emu/                  portable C99, no Zephyr, no board
    riscv_cpu.c         TinyEMU, patched — see §7
    riscv_cpu_template.h, riscv_cpu_priv.h, riscv_cpu.h, cutils.h, iomem.h
    shim/byteswap.h     the one header bare metal is missing
    rv_iomem.c          allocation-free replacement for TinyEMU's iomem.c
    rv_hostsbi.c        SBI + initial machine state, in host C
    rv_machine.c        memory map, devices, run loop
    rv_fdt.c            devicetree generator
    rv_platform.h       everything emu/ needs from the outside
  src/
    platform_zephyr.c   the only file that knows this is an EK-RA8D1
    main.c              image container, loader, banner
    testrom.h           generated: the smoke payload, compiled in
  host/                 the same emu/ built for macOS, plus the RV32 tests
  tools/                image packing and pushing
  boards/ek_ra8d1.*     devicetree overlay and Kconfig
```

`emu/` compiles unchanged for both targets. `host/Makefile` uses the same five
`-D` flags the Zephyr `CMakeLists.txt` does, on purpose: a change that breaks
on the board breaks on the Mac in two seconds instead of a flash cycle on
hardware someone else is using.

### Footprint, measured

`west build -p always -b ek_ra8d1`, no warnings from any file in this tree.
Two configurations, because `TINYEMU_FLEN` is the one build knob that matters
(§6):

| | **FLEN=64** (default) | FLEN=0 |
|---|---:|---:|
| FLASH | **89,172 B / 2016 KB — 4.32%** | ~74,700 B — 3.7% |
| RAM | **25,560 B / 896 KB — 2.79%** | 25,296 B — 2.75% |
| SDRAM | 0 B (handed to the guest at runtime) | 0 B |

Per object, at the default FLEN=64, with the FLEN=0 figure in brackets where
it differs:

| object | text | bss |
|---|---:|---:|
| `emu/riscv_cpu.c` | 12,728 (8,652) | 6,664 (6,400) |
| `emu/softfp.c` | 12,128 (absent) | 0 |
| `emu/rv_fdt.c` | 4,129 | 0 |
| `emu/rv_machine.c` | 2,627 | 432 |
| `emu/rv_virtio.c` | 1,576 | 96 |
| `emu/rv_iomem.c` | 848 | 2,592 |
| `emu/rv_hostsbi.c` | 636 | 8 |
| `src/main.c` (incl. 2,972 B `testrom`) | 4,718 | 0 |
| `src/platform_zephyr.c` | 1,145 | 12 |
| **app total** | **40,535** | **9,804** |

The rest is Zephyr, picolibc and the Renesas drivers.

The read-only virtio-blk device is **1,576 B of text and 96 B of bss** — see
§5a for why it is written rather than vendored, and for the comparison with
TinyEMU's own.

**Floating point costs 14,472 B of flash and 264 B of RAM**, and no measurable
speed: booting the same kernel to the same point gave 193.7-194.6 MIPS at
FLEN=0 and 185.1-204.2 MIPS at FLEN=64 on the same Mac, which is one noise
band. 4.29% of the part either way.

The integer core is 8,652 bytes against the 8,392 the spike measured; the
260 B difference is the `time` CSR and the SBI hook from §7. Its 6,400 B of
`.bss` is the CPU state, almost entirely the three 256-entry TLBs
(3 x 256 x 8 = 6,144 B); FLEN=64 adds the 32 FP registers (256 B).
`rv_iomem.c`'s 2,592 B is the 32-entry `PhysMemoryRange` table, upstream's
fixed `PHYS_MEM_RANGE_MAX`; six entries are used. Neither is worth shrinking
against 896 KB.

---

## 2. Memory map

Guest physical, chosen to keep the mini-rv32ima port's layout so the paravirt
bridge and its guest driver carry over unchanged:

| address | size | what |
|---|---|---|
| `0x10000000` | 256 B | 8250 UART, byte-wide registers, PLIC source 1 |
| `0x10001000` | 4 KB | **virtio-mmio**, read-only block device, PLIC source 2 |
| `0x11000000` | 64 KB | CLINT — `msip`, `mtimecmp`, `mtime` |
| `0x11100000` | 4 KB | syscon — poweroff `0x5555`, reboot `0x7777` |
| `0x11200000` | 4 KB | **paravirt I/O**, register-for-register as `notes/05` |
| `0x40100000` | 4 MB | PLIC, one context, 31 sources |
| `0x80000000` | 64 MB | RAM |

Host side:

| address | what |
|---|---|
| `0x68000000` | 64 MB SDRAM — the whole of it is guest RAM |
| `0x90000000` | 64 MB octo-SPI NOR, memory-mapped for reads |
| `0x90040000` | kernel slot, 8 MB, magic `RA8LINUX` |
| `0x90840000` | rootfs slot, 55.75 MB, magic `RA8ROOTF`, **read in place** |

Inside guest RAM:

```
0x80000000   kernel Image, entered in S-mode
(4 KB aligned after it)   initrd, if any
0x83f00000   devicetree (RAM top − 1 MB)
```

The kernel goes at the very base, not at the +2 MB offset a QEMU/OpenSBI
system uses. That offset exists to leave room for M-mode firmware, and there
is none here. `0x80000000` is 4 MB aligned, which is what an Sv32 kernel needs
for its megapage-mapped text.

The PLIC is at TinyEMU's `0x40100000` rather than somewhere in the
`0x11xxxxxx` block, because mini-rv32ima had no PLIC to inherit a location
from and matching upstream costs nothing.

**The flash slots are not ours.** They are the offsets, magics and 16-byte
header format the rvlinux app on this board already writes and its TCP pusher
already tests end to end. Adopting them byte-for-byte means an image pushed
with the working tool boots under this app unchanged, and it is why this port
has no image container and no loader of its own — both were written, then
deleted in favour of the tested path. `src/platform_zephyr.c` BUILD_ASSERTs
the geometry so a well-meant edit to the overlay cannot silently break it.

**The rootfs slot is never copied.** It is handed to the block device as a
pointer into the memory-mapped OSPI window. That is not an optimisation: the
kernel already reserves 26 MB of the 64 MB SDRAM, so a 55 MB rootfs copied
into RAM does not fit by a wide margin. Read in place it costs zero SDRAM.

---

## 3. SBI: implemented in host C, not loaded as a blob

**Decision: a host-side SBI in `emu/rv_hostsbi.c`. No OpenSBI, no BBL.**

MMU Linux boots in S-mode and needs SBI underneath it. The two options were:

**(a) Load OpenSBI or BBL as a bios blob**, which is what upstream TinyEMU
does — `riscv_machine.c` puts the bios at RAM base, the kernel at +2 MB, and
starts the CPU in M-mode at `0x1000` in a small boot ROM that jumps to it.

**(b) Service the guest's supervisor `ecall` on the host**, in the emulator,
and start the CPU already in S-mode.

We chose (b), for four reasons:

1. **There is no blob.** An rv32 OpenSBI `fw_jump` is another 100 KB artifact
   to build, version, store in flash and keep in step with the kernel. On a
   board where the image push is already the slow part of the loop, that is
   real cost for no capability.
2. **It is free at runtime.** OpenSBI services a timer or console call by
   executing emulated M-mode instructions — trap in, decode, act, `mret`.
   Here the same call is a C switch statement running at 480 MHz native. On an
   interpreter managing single-digit MIPS, moving the console and the timer
   off the emulated path is the difference between a boot log that streams and
   one that crawls.
3. **The console works before the kernel has a driver.** `earlycon=sbi` goes
   straight to `uart_poll_out()`. Bring-up on this board has repeatedly turned
   on being able to see the first hundred lines of a boot.
4. **It is small and checkable.** The whole thing is ~330 lines including
   comments, and `host/tests/smoke.c` exercises it directly.

The cost is honest: we hand-wrote SBI semantics instead of inheriting them.
That is mitigated by (b) being a much smaller surface than the privileged-spec
work path A would have needed — the page-table walk, S-mode CSRs, delegation
and the TLB all still come from TinyEMU, which is the part that was expensive.

### What SBI is implemented

- **v0.1 legacy, complete**: `set_timer`, `console_putchar`, `console_getchar`,
  `clear_ipi`, `send_ipi`, `remote_fence_i`, `remote_sfence_vma[_asid]`,
  `shutdown`. This is what a kernel with `CONFIG_RISCV_SBI_V01` uses.
- **BASE (0x10)**: spec version reported as **1.0**, impl id `0x52413844`
  (`'RA8D'` — deliberately not claiming to be OpenSBI, since Linux prints it),
  `probe_extension`, and `mvendorid`/`marchid`/`mimpid` as 0.
- **TIME**, **IPI**, **RFNC**, **SRST**, **DBCN**.
- **HSM is deliberately absent.** Advertising it would oblige us to start
  secondary harts and there is exactly one. `probe_extension` returns 0 and the
  kernel takes the path it takes on any single-cpu devicetree.

The RFENCE calls are no-ops rather than stubs pretending to be correct: they
are *remote* fences on a machine with no remote hart, and the calling hart has
already fenced itself before it gets here.

An unrecognised extension is answered with `SBI_ERR_NOT_SUPPORTED`, never
declined. Declining would deliver a cause-9 trap to a kernel with no handler
for one, which is a strictly worse failure than an error code.

### Implemented vs stubbed vs absent — the whole list

Recorded exhaustively because "is our SBI complete enough" is the first
hypothesis when a guest hangs early with no console output, and that exact
failure mode has already cost this project most of a day. `sbi_probe_extension`
answers from this same table (`sbi_ext_supported()` in `rv_hostsbi.c`), so
what the kernel is told and what actually works cannot drift apart.

| EID | name | fid | state | notes |
|---|---|---|---|---|
| `0x00` | legacy `set_timer` | — | **real** | arms the machine deadline, clears `STIP` |
| `0x01` | legacy `console_putchar` | — | **real** | straight to `plat_putc()` |
| `0x02` | legacy `console_getchar` | — | **real** | returns the byte, or −1 when idle |
| `0x03` | legacy `clear_ipi` | — | **real** | clears `SSIP` |
| `0x04` | legacy `send_ipi` | — | **real** | sets `SSIP`; the only hart is this one |
| `0x05` | legacy `remote_fence_i` | — | **no-op** | correct: no remote hart |
| `0x06`/`0x07` | legacy `remote_sfence_vma[_asid]` | — | **no-op** | as above |
| `0x08` | legacy `shutdown` | — | **real** | stops the run loop |
| `0x10` | BASE | 0 | **real** | `get_spec_version` → `0x01000000` (**v1.0**) |
| | | 1 | **real** | `get_impl_id` → `0x52413844` (`'RA8D'`) |
| | | 2 | **real** | `get_impl_version` → 1 |
| | | 3 | **real** | `probe_extension`, from this table |
| | | 4–6 | **real** | `mvendorid`/`marchid`/`mimpid`, legitimately 0 |
| `TIME` | timer | 0 | **real** | `set_timer`, 64-bit as an RV32 register pair |
| `sPI` | IPI | 0 | **real** | `send_ipi` → `SSIP` |
| `RFNC` | remote fence | 0–6 | **no-op** | see above; fid > 6 → NOT_SUPPORTED |
| `SRST` | system reset | 0 | **real** | type 0 poweroff, 1/2 reboot |
| `DBCN` | debug console | 0 | **real** | `console_write`, guest-*physical* buffer |
| | | 1 | **real** | `console_read` |
| | | 2 | **real** | `console_write_byte` |
| `HSM` | hart state | — | **absent** | one hart; probe returns 0 by design |
| `PMU`, `SUSP`, `CPPC`, anything else | | | **absent** | probe returns 0; a call gets NOT_SUPPORTED |

**Both console paths exist.** DBCN is implemented, not only the legacy
`console_putchar`, because modern kernels prefer DBCN and fall back. Verified
in the boot log: `earlycon=sbi` produces output from the first line, and the
smoke payload exercises DBCN's `console_write` explicitly.

What the kernel makes of all this, from an actual boot:

```
[    0.000000] SBI specification v1.0 detected
[    0.000000] SBI implementation ID=0x52413844 Version=0x1
[    0.000000] SBI TIME extension detected
[    0.000000] SBI IPI extension detected
[    0.000000] SBI RFENCE extension detected
[    0.000000] SBI SRST extension detected
```

and later, benignly, `cpuidle-riscv-sbi: HSM suspend not available` — the
kernel probing for HSM, being told no, and carrying on. Across a full boot to
userspace the counter in `riscv_host_sbi_stats()` reports **0 unsupported
calls**, so nothing the kernel actually wanted is missing.

### The fallback: OpenSBI, and why we are not using it

The Buildroot MMU build produces one, and it is small:

| artifact | bytes |
|---|---:|
| `fw_jump.bin` | 123,080 |
| `fw_dynamic.bin` | 123,080 |
| `fw_jump.elf` | 999,420 |
| `fw_dynamic.elf` | 999,796 |

123 KB of a 64 MB OSPI is nothing, and it is upstream, spec-complete and
maintained by other people. That is a genuinely strong argument and it is why
this is recorded as a live fallback rather than a rejected idea.

**Staying with the host SBI, because it is now working against a real kernel**,
and because the reasons above still hold: no second blob for the image loader
to place or the OSPI map to carry, no version skew between firmware and kernel,
console and timer off the emulated instruction path, and timer/IPI wired
straight to Zephyr primitives.

**How to switch, if it ever becomes the right call.** The change is contained:
drop `CONFIG_HOST_SBI`, which disables the five-line hook in `raise_exception2`
and the `time`/`timeh` CSRs (OpenSBI traps and emulates `rdtime` itself, which
is what upstream TinyEMU's `invalid_csr` comment is about). Then place
`fw_jump.bin` at `0x80000000`, move the kernel to `0x80400000` — that is the
rv32 `text_offset` this port otherwise ignores, see §4 — and enter in M-mode at
the firmware rather than in S-mode at the kernel, i.e. call something like
`riscv_cpu_boot_mmode()` instead of `riscv_cpu_boot_smode()`. The image
container would need a third payload slot. Nothing in `rv_machine.c`,
`rv_fdt.c` or `rv_iomem.c` changes, except that the PLIC and the timer would
want to raise `MEIP`/`MTIP` again as upstream does, since there would finally
be an M-mode handler for them.

A real rv32 OpenSBI build to diff behaviour against lives at
`/br/mmu/images/fw_jump.elf` in the `br` container (OpenSBI 1.2, built for
`qemu_riscv32_virt_defconfig`).

### Initial machine state

`riscv_cpu_boot_smode()` leaves the CPU as a bootloader would:

| | |
|---|---|
| privilege | S |
| `pc` | `0x80000000` |
| `a0`, `a1` | hartid (0), devicetree address |
| `satp` | 0 (paging off; the kernel turns it on) |
| `medeleg` | `0xf1ff` — everything except causes 9, 10, 11 |
| `mideleg` | `SSIP | STIP | SEIP` |
| `mcounteren` | `0x7` — cycle, **time**, instret readable below M |

Causes 9 (ecall from S) and 11 (ecall from M) are left undelegated on purpose.
Cause 9 is ours — the SBI intercept runs before delegation is even considered
— and leaving it undelegated means a bug in that intercept shows up as an
immediate stop rather than as a baffling trap inside the kernel's own handler.

Two consequences of having no M-mode run through the machine layer:

- **Only `SEIP` is ever raised, never `MEIP`.** Upstream's PLIC raises both,
  because BBL is there to field the M-mode one. Here a cause-11 interrupt
  cannot be delegated by `mideleg` and would be delivered straight to an
  `mtvec` of zero.
- **Same for the timer**: `STIP`, never `MTIP`.

---

## 4. Loading the kernel, and the two bugs that found

**The initrd must not go immediately after the kernel file.** A RISC-V Linux
Image carries a 64-byte header whose `image_size` covers the kernel's `.bss`
as well as its loaded bytes. The Buildroot kernel is a 25,923,072-byte file
declaring `image_size` 26,259,456 — a 336 KB difference — and the kernel
reserves `[_start, _end]` from memblock, so anything placed at file-end lands
inside memory it has already claimed:

```
[    0.000000] INITRD: 0x818b9000+0x002eb000 overlaps in-use memory region
[    0.000000]  - disabling initrd
```

Fixed two ways, belt and braces: `rv_kernel_footprint()` parses the header and
uses `image_size` when the `RSC\x05` magic is present, and the initrd is now
placed **high** — just below the devicetree at the top of RAM — rather than
after the kernel. Either fix alone would have done; together the layout is
right even for a payload with no header.

**`text_offset` is deliberately ignored.** The header asks for the image at
RAM base + 4 MB on rv32. That offset exists to leave room for M-mode firmware
this machine does not have, and Linux computes its load address at runtime in
`setup_vm()` requiring only 4 MB alignment, which `RV_RAM_BASE` has. Spending
4 MB of a 64 MB guest to honour it buys nothing. Verified by booting 6.1.44 at
`0x80000000`. If OpenSBI is ever adopted (§3) this reverses.

The resulting layout:

```
0x80000000                  kernel, entered in S-mode
...                         (zeroed)
top - 1 MB - initrd_size    initrd, page-aligned
0x83f00000 (top - 1 MB)     devicetree
```

**Second bug: a bounded run could never end against an idle guest.**
`rv_machine_run_bounded()` originally took only an instruction limit, and a
guest sitting in WFI retires no instructions, so the bound never advanced —
the harness hung instead of timing out. It now takes an instruction limit and
a wall-clock limit, either of which may be 0. Only the test harness uses
bounds; the app runs unbounded.

---

## 5. The rest of the machine

**RAM** is registered against the SDRAM pointer the platform hands over.
Upstream's `iomem.c` allocates it with `mallocz`, which is not available and
would not be able to produce 64 MB if it were; `rv_iomem.c` adds
`rv_register_ram_ptr()` and drops the allocator entirely.

**Timer.** `mtime` is `plat_now_us()` and the devicetree declares
`timebase-frequency = <1000000>`, so the guest's tick and the host's
microsecond are the same thing and there is no conversion to get wrong. SBI
`set_timer` records the deadline and clears any pending `STIP`, as the spec
requires; the run loop raises `STIP` when the deadline passes and disarms.

**Console.** An 8250 at `0x10000000` with a 64-byte receive ring, drained from
`plat_getc()` once per instruction slice. `LSR.THRE` and `TEMT` are always set
— `plat_putc()` has completed by the time it returns — so with `IER.THRI`
enabled the interrupt is continuously asserted, which is what real hardware
does with an empty FIFO and which the Linux driver masks off when its queue
drains.

**PLIC.** The same minimal model as upstream: priority, pending and enable
registers are accepted and ignored, so every source behaves as enabled, and
only claim/complete does real work. Linux writes the ignored registers on the
way up and then only claims and completes, so the shortcut is invisible to it.
The devicetree lists **one** context (S-mode external), which fixes the context
index at 0 and therefore the hart registers at `PLIC + 0x200000`, which is what
the model decodes.

**Devicetree, generated at boot** by `rv_fdt.c` rather than stored next to the
kernel. Everything in it that can vary — how much SDRAM the board reported,
where the initrd landed, the command line — is only known at that point, and
it removes the class of failure the mini-rv32ima port lived with where a stale
compiled-in DTB silently disagreed with the machine (see the `ramsweep` and
`newimage` logs under `ra8d1-linux/notes/`). The generated blob is checked by
`dtc`, not just by us: `host/tinyemu-host <payload> -d out.dtb` writes it out
and `dtc -I dtb -O dts out.dtb` parses it with no warnings.

**Paravirt I/O** at `0x11200000` is preserved register-for-register from
`ra8d1-linux/notes/05-paravirt-io.md`: same ID, same register offsets, same
command codes, same 256-byte word-addressed data window, same eight GPIO pins
and the same `iic1` bus. The guest driver in `ra8d1-linux/guest/pv-io.c` should
bind unchanged. One improvement: TinyEMU's device callbacks *are* told the
access width, where mini-rv32ima's MMIO hook was not, so the "32-bit accesses
only" rule that the ABI could previously only declare is now enforced — a byte
access reads zero instead of a plausible-looking value.

---

## 5a. Storage: read-only virtio-blk, reading in place from flash

**Written rather than vendored from TinyEMU's `virtio.c`**, which was the
obvious choice and which I did not take. The reasons, since they go against
the grain:

- `virtio.h` pulls `<sys/select.h>`, `pci.h` and `fs.h`. The first does not
  exist on bare metal; the other two drag in the PCI transport and the whole
  9p filesystem client, none of which this machine has.
- We need roughly 15% of its 2,650 lines: one transport, one device, one
  direction.
- Both problems found in that code become structural here rather than patched.
  `virtio_block_init` (virtio.c:1117) never sets `device_features`, so the
  guest sees a writable disk; this device offers `VIRTIO_BLK_F_RO` and has no
  write path to forget to disable. The per-request `malloc` bounce buffer
  (virtio.c:1084) **does not exist**, because the backing store is already a
  readable host pointer — the memory-mapped OSPI window — so data goes
  straight from flash into the guest's descriptor buffers with no intermediate
  copy at all. There was no allocation to make static.

Result: **1,576 B of text and 96 B of bss**, against the 5,166 B measured for
TinyEMU's block-only build after `--gc-sections`.

Scope, stated so the limits are not discovered later: modern virtio-mmio only
(version 2, `VIRTIO_F_VERSION_1`), one virtqueue, split rings, no indirect
descriptors, no event index, no write path. Every one of those is a feature
simply not offered, so a conforming driver never uses it.

Everything the guest hands over — descriptor table, available ring, used ring,
every data buffer — is a guest *physical* address resolved through
`rv_guest_phys_ptr()`, which bounds-checks against RAM. A descriptor pointing
outside RAM fails the request with `VIRTIO_BLK_S_IOERR` rather than the host.

**ext2, not EROFS.** The plan was EROFS; checking the guest kernel rather than
assuming showed it does not have it. Registered filesystem names in the Image:

```
ext4 1, ext2 1, iso9660 1, vfat 1, ramfs 1, proc 1, sysfs 1
erofs 0, squashfs 0, romfs 0, cramfs 0
```

The only `EROFS` string in the whole 25.9 MB image is `-EROFS`, the errno text
for "Read-only file system". `virtio_blk` appears 9 times and `virtio-mmio` 3,
so the transport half of the plan was fine. ext2 gives the same
read-in-place-from-flash property with no kernel rebuild, and Buildroot already
produces `rootfs.ext2`. EROFS can come later bundled with a kernel trim.

---

## 6. Float: the guest userspace is hard-float

**`TINYEMU_FLEN` defaults to 64, and it has to.**

The Buildroot MMU guest is built for the **ilp32d** ABI:

```
busybox:      Flags: 0x4, double-float ABI
              Tag_RISCV_arch: "rv32i2p1_m2p0_a2p1_f2p2_d2p2_zicsr2p0_..."
interpreter:  /lib/ld-linux-riscv32-ilp32d.so.1
vmlinux:      Flags: 0x1, RVC, soft-float ABI
```

The kernel is soft-float and boots fine on an rv32ima core. **Userspace does
not.** With `FLEN=0` the port booted the kernel perfectly and then:

```
[    0.332812] init[1]: unhandled signal 4 code 0x1 at 0x00010cf8
[    0.347288] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000004
```

Signal 4 is SIGILL, on the first F or D instruction in glibc. Linux on RISC-V
does not emulate absent FP hardware.

Building with `FLEN=64` compiles TinyEMU's `softfp.c` and the F/D paths in the
instruction template, and userspace runs. The cost is 14.5 KB of flash and no
measurable speed (§1).

The devicetree's `riscv,isa` is now **derived from `misa`** rather than
hardcoded, so the two cannot disagree — at FLEN=64 it emits `rv32acdfimsu` and
the kernel reports `riscv: base ISA extensions acdfim`; at FLEN=0, `rv32acimsu`
and `acim`. This matters more than it looks: the kernel decides whether to save
and restore FP context from that string, so a build with FP enabled and an isa
string that forgot to say so would execute F and D instructions correctly and
silently corrupt them across every context switch.

**The alternative is a soft-float guest.** Rebuilding Buildroot with
`BR2_RISCV_ABI_ILP32` and no F/D would let us go back to `FLEN=0` and save the
14.5 KB. That is a guest-side decision, not an emulator one; the knob is here
either way (`-DTINYEMU_FLEN=0`, or `make FLEN=0` in `host/`).

---

## 7. Patches to the vendored TinyEMU sources

Six, all small, all marked in-file with a comment beginning "RA8D1 port".
Reproduce them by re-extracting the tarball and re-applying.

| file | change | why |
|---|---|---|
| `shim/byteswap.h` | new, 8 lines | `cutils.h:87` includes `<byteswap.h>`, a glibc-ism. Worth upstreaming. |
| `cutils.h:29` | `#ifndef` around `likely`/`unlikely`/`__maybe_unused` | Zephyr's `sys/util_macro.h` defines all three identically; without the guards every TU that includes both warns. |
| `riscv_cpu.c` `COUNTEREN_MASK` | add bit 1 (TM) | Upstream omits it because it does not implement `time` at all. See the next row. |
| `riscv_cpu.c` `csr_read` | implement `0xc01`/`0xc81` (`time`/`timeh`) | Upstream leaves these out for M-mode firmware to trap and emulate — its own comment at the `invalid_csr` label says "the 'time' counter is usually emulated". With no firmware, and Linux reading `time` on every tick and in the vDSO, they have to be answered directly. |
| `riscv_cpu.c` `raise_exception2` | 5-line `CONFIG_HOST_SBI` hook | §3. Returning without delivering the trap leaves the interpreter at its `exception:` label, which falls through to `done_interp`; `s->pc` is stepped past the `ecall` (never compressed, so always +4). |
| `riscv_cpu.c` `riscv_cpu_init` | `\|\| CONFIG_RISCV_MAX_XLEN == 32` on the EMSCRIPTEN branch | Upstream names `riscv_cpu_class64` unconditionally, which works only because its Makefile links all three instantiations. We compile one. |
| `riscv_cpu.c` `riscv_cpu_end` | `#ifdef` → `#ifndef` around `free(s)` | Upstream frees the pointer precisely when it is the address of a static object. Never live here (nothing calls it) but it would arm `free(&static)` for whoever calls it first. **This is an upstream bug.** |

`softfp.c`, `softfp.h`, `softfp_template.h`, `softfp_template_icvt.h` and
`riscv_cpu_fp_template.h` are vendored **unpatched** — they are needed at
FLEN > 0 (§6) and compiled clean for both targets as they are.

Not patched, deliberately: the 26 debug `printf`s, the 5 `abort`s and the
`fopen("/tmp/riscemu.log")` the spike found are all inside `#ifdef DUMP_*` /
`CONFIG_LOGFILE` blocks that are off, so they cost nothing and diverging from
upstream to delete them would only make the next re-vendor harder.

The single `mallocz` in the core is gone without a patch: upstream already
guards a static `RISCVCPUState` behind `USE_GLOBAL_STATE` for its emscripten
build, and we define it. **There is no dynamic allocation anywhere in the
emulator.**

`cutils.c` is not compiled at all — `riscv_cpu.c` uses exactly two things from
it, `ctz32` (a static inline in the header) and `mallocz` (gone). `softfp.c` is
compiled only when `TINYEMU_FLEN` is non-zero.

---

## 8. What is verified, and how

`host/tests/smoke.c` is an RV32 S-mode payload built with the Buildroot
riscv32 toolchain in the `br` container. It runs under `host/tinyemu-host` and
under the Zephyr app (compiled in as `src/testrom.h`, booted when the image
slot is empty). Every line below is output from an actual run, not a
prediction:

```
=== ra8d1-tinyemu smoke test ===
hartid is 0 ok
dtb at 0x83f00000 magic 0xd00dfeed size 0x000006b9
dtb magic ok
dtb size sane ok
sbi spec 0x01000000
sbi spec version 1.0 ok
sbi probe TIME ok
sbi probe HSM absent ok
dbcn write ok
dbcn console_write ok
time 0x00000034 -> 0x000000bb
time advances ok
paging on, satp 0x80080005
sv32 alias reads the same word ok
sv32 alias writes through ok
fault scause 0x0000000d stval 0x50000000
load page fault delivered to stvec ok
scause is 13 (load page fault) ok
stval is the faulting address ok
sbi set_timer accepted ok
timer scause 0x80000005
supervisor timer interrupt delivered ok
scause is interrupt 5 ok
=== ALL CHECKS PASSED ===
```

What each group actually establishes:

- **Sv32 is applied, not ignored.** The payload maps the same 4 MB of physical
  memory at two different virtual addresses and shows a store through one
  visible through the other. If translation were not honoured the aliased
  address would fault or read nothing.
- **Delegation works.** A load from an unmapped page arrives at `stvec` with
  `scause` 13 and `stval` set, which only happens if `medeleg` is right.
- **The timer path works end to end**: SBI `set_timer`, the machine's deadline
  check, `STIP`, `mideleg`, and `sie`/`sstatus.SIE` gating.
- **The `time` CSR patch works**, from S-mode, gated by `mcounteren`.

The devicetree is separately checked by `dtc`, which parses the generated blob
with zero warnings.

### And then: real Linux, and then the point of the whole project

`/br/mmu/images/Image` — Buildroot's rv32 Sv32 **Linux 6.1.44** — boots on this
machine layer, under `host/tinyemu-host`, all the way to userspace. Selected
lines, all from actual runs:

```
[    0.000000] Linux version 6.1.44 ... riscv32-buildroot-linux-gnu-gcc 12.3.0 #1 SMP
[    0.000000] Machine model: ra8d1-tinyemu
[    0.000000] Zone ranges:  Normal [mem 0x0000000080000000-0x0000000083ffffff]
[    0.000000] SBI specification v1.0 detected
[    0.000000] SBI implementation ID=0x52413844 Version=0x1
[    0.000000] riscv: base ISA extensions acdfim
[    0.000000] plic: plic@40100000: mapped 31 interrupts with 1 handlers for 1 contexts.
[    0.000000] riscv-timer: Registering clocksource cpuid [0] hartid [0]
[    0.000002] sched_clock: 64 bits at 1000kHz, resolution 1000ns
[    0.000466] Calibrating delay loop (skipped) ... 2.00 BogoMIPS (lpj=4000)
[    0.087365] Unpacking initramfs...
[    0.261936] 10000000.serial: ttyS0 at MMIO 0x10000000 (irq = 1, base_baud = 115200) is a 16550A
[    0.266258] printk: console [ttyS0] enabled
[    0.342614] Freeing unused kernel image (initmem) memory: 4164K
[    0.331375] Run /init as init process

=== userspace reached ===
uname: Linux 6.1.44 riscv32
```

What that adds over the smoke payload, concretely:

- **The 8250 and the PLIC work under the real Linux drivers**, not just under
  our own test code. `console [ttyS0] enabled` at t=0.266 means every log line
  after it came through the emulated UART and the emulated interrupt
  controller, not through SBI earlycon.
- **The whole Sv32 kernel path**: `setup_vm`, the linear map, vmalloc, the
  fixmap, `debug_vm_pgtable`'s own page-table-helper self-test, module and slab
  init, and 4 MB hugepages registered.
- **The timer as a clocksource and a scheduler clock**, at the 1 MHz timebase
  the generated devicetree advertises.
- **Userspace under Sv32** — a static rv32 binary running, making syscalls, and
  `uname()` reporting `riscv32`.
- **Both poweroff paths coexist**, visibly: `syscon-poweroff poweroff:
  pm_power_off already claimed for sbi_srst_power_off` is the kernel finding
  the SBI SRST handler first and declining our syscon node. Both are wired; SBI
  wins, which is the right precedence.
- **0 unsupported SBI calls** across a full boot.

Throughput on the Mac is 185–205 MIPS for a full kernel boot. That is a host
number and says nothing about the M85.

---

## 8a. On the board

Flashed 2026-08-07 with `probe-rs` via `flash.sh`, which is `rvlinux/flash.sh`
unchanged apart from the scratch filename. App: 90,968 B flash, 25,560 B RAM.

**First boot found a real bug**, which is the argument for flashing rather than
reasoning. The kernel slot holds the rvlinux guest, and that is a *valid rv32
Linux Image* with a good CRC and the right slot magic — it is simply built
`CONFIG_RISCV_M_MODE`. The app loaded it, entered it in S-mode, and hung with
`--- guest output follows ---` and nothing after. The Image-magic check I had
added in anticipation could not catch it, because the magic is correct.

The fix is `text_offset`: `arch/riscv/kernel/head.S` sets it to 0 for an
M-mode build and 0x400000 for rv32 otherwise, so it is a reliable marker. The
app now says so instead of hanging:

```
kernel: slot holds 3476752 B, a valid rv32 Image but an M-mode (nommu) build
        -- that is the rvlinux guest, and this machine boots S-mode kernels
kernel: no valid image in flash; running the built-in self-test instead
```

### The self-test, on silicon

Every check that passed on the Mac passes on the board:

```
ra8d1-tinyemu: rv32 Sv32 Linux host
host: cortex-m85 @ 480 MHz, sdram 64 MB
guest: ram 0x80000000+64 MB, dtb at 0x83f00000 (1697 B)
--- guest output follows ---

=== ra8d1-tinyemu smoke test ===
hartid is 0 ok
dtb magic ok / dtb size sane ok
sbi spec version 1.0 ok / sbi probe TIME ok / sbi probe HSM absent ok
dbcn console_write ok
time advances ok
paging on, satp 0x80080006
sv32 alias reads the same word ok
sv32 alias writes through ok
fault scause 0x0000000d stval 0x50000000
load page fault delivered to stvec ok / scause is 13 ok / stval ok
supervisor timer interrupt delivered ok / scause is interrupt 5 ok
benchmarking (paging on)...
  3200016 insns in 208195 us = 15.37 MIPS
console input test: type for 8 s (q ends it early)
  got byte 0x00000048 'H'
  got byte 0x00000069 'i'
  got byte 0x00000071 'q'
console input: 3 bytes received
=== ALL CHECKS PASSED ===

--- guest stopped (poweroff) ---
insns 18534749 in 1298 ms, sbi calls 4723 (0 unsupported), blk requests 0
```

### The two numbers that only hardware could give

**Throughput: 15.37 MIPS**, measured with Sv32 paging on, guest RAM in
external SDRAM, D-cache enabled. Self-measured rather than estimated: the
payload reads `instret` and `time` around the loop, so it is two counters
divided, not a loop count times an assumed cost. The whole-run figure of
18,534,749 instructions in 1,298 ms — 14.3 MIPS including the self-test's
console I/O — is an independent cross-check that agrees.

The same benchmark is **293.09 MIPS** under `tinyemu-host` on an arm64 Mac, so
the M85 comes in **19x slower** than the development machine. Worth writing
down so nobody reads a Mac number as a board number again.

**Caveat, stated as the lead's own benchmark note states it:** this loop
touches 64 words in a single mapped page, so it is a 100% TLB-hit,
cache-resident workload and therefore the best case. Real kernel and userspace
code will take page-walk misses and SDRAM misses this does not. The true
figure is below 15.37, not above it.

What that implies for a guest boot, as an estimate and clearly labelled as one:
reaching userspace took ~90 M instructions under the host harness, which at
15.37 MIPS is **about 6 seconds**, plus the block reads to mount the rootfs.
Against the nommu port's 2.4 s that is slower but entirely usable. It is an
extrapolation until a kernel actually boots on the board.

**Console input works.** This was the open question, and it is closed: bytes
typed at the physical UART reach the guest through `uart_poll_in()`, the 8250
model's receive ring and the PLIC. `H`, `i` and `q` all arrived and the `q`
ended the test early.

### Other things the board confirmed

- **SDRAM**: 64 MB reported and handed to the guest, `CONFIG_MEMC` correct.
- **The OSPI window reads correctly with the D-cache on**, which was an open
  risk. The app CRC'd 3,476,752 bytes out of memory-mapped flash in 57 ms —
  **61 MB/s** — and got a value matching what the pusher wrote.
- **The block device attaches to a real slot.** On the first boot the rootfs
  slot held the lead's 512 KB test push and came up as `vda 1024 sectors`.
- **SBI poweroff works** end to end: the guest called SRST, the run loop
  stopped, and the app printed its statistics.
- **0 unsupported SBI calls**, on hardware as on the host.

---

## 8b. Linux on the board

2026-08-07. Pushed the trimmed kernel (6,543,776 B) and a rebuilt rootfs
(37,748,736 B) into the OSPI slots with the rvlinux TCP pusher, reflashed this
app, and booted.

```
ra8d1-tinyemu: rv32 Sv32 Linux host
host: cortex-m85 @ 480 MHz, sdram 64 MB
kernel: 6543776 B, crc ok in 107 ms
rootfs: 36 MB, read in place from OSPI
guest: ram 0x80000000+64 MB, dtb at 0x83f00000 (1869 B), vda 73728 sectors
--- guest output follows ---
[    0.000000] Linux version 6.1.44 ... #1 SMP
[    0.000000] Machine model: ra8d1-tinyemu
[    0.000000] SBI specification v1.0 detected
[    0.000000] SBI implementation ID=0x52413844 Version=0x1
[    5.461658] 10000000.serial: ttyS0 at MMIO 0x10000000 (irq = 1, base_baud = 115200) is a 16550A
[    5.779448] virtio_blk virtio0: [vda] 73728 512-byte logical blocks (37.7 MB/36.0 MiB)
[    6.490640] EXT4-fs (vda): mounting ext2 file system using the ext4 subsystem
[    6.560388] VFS: Mounted root (ext2 filesystem) readonly on device 254:0.
[    6.579862] Run /sbin/init as init process

Welcome to Buildroot
buildroot login: root
# python3 -c "import ctypes,sys;print('PY',sys.version.split()[0],ctypes.CDLL(None))"
PY 3.11.6 <CDLL 'None', handle 9471ead0 at 0x9409edb0>
```

### Timing, from the probe-rs reset

| milestone | elapsed | segment |
|---|---:|---:|
| app banner | 0.10 s | |
| guest first instruction | 0.72 s | 0.62 s app init, incl. the kernel CRC |
| `Linux version` | 0.92 s | |
| rootfs mounted | 7.64 s | **5.66 s of kernel boot** |
| `buildroot login:` | **34.48 s** | 27.90 s of userspace init |

The kernel half matches the prediction: §8a estimated ~6 s to userspace from
90 M instructions at 15.37 MIPS, and the measurement is 5.66 s. That is the
emulator behaving exactly as characterised.

**The userspace half is 28 s and most of it is waste.** Buildroot's network
init sits in `Waiting for interface eth0 to appear...............timeout!`,
which is ~15 s of a machine that has no network device at all. Removing that
one script should take the boot to roughly 20 s. Not done here because the
rootfs belongs to the guest build, not to this port.

### Block device throughput in service

`dd if=/dev/vda of=/dev/null bs=64k count=64` — 4 MiB in 1.18 s real, of which
0.99 s is `sys`. **3.4 MB/s.**

That number is *emulation-bound, not flash-bound*, and the distinction matters
because the two figures get confused easily:

- The OSPI window itself does **61 MB/s**, measured by the app CRC'ing 6.5 MB
  of kernel out of it in 107 ms before the guest starts.
- The guest sees 3.4 MB/s because every sector goes through the virtio ring,
  the guest kernel's block layer and the interpreter. The 0.99 s of `sys` time
  against 1.18 s wall says almost all of it is emulated instructions.

So 61 MB/s is the floor of what the flash can do and 3.4 MB/s is the ceiling of
what a guest can get through this emulator. Neither is a property of the OSPI.

---

## 8c. pv-io.c built in, and the push path that blocked the test

**Built and host-verified; NOT tested on the board.** The reason is not this
port — see below.

### What was done

`guest-runtime`'s fixed `pv-io.c` (from `/br/mmu-pv`, with the
`MODULE_LICENSE`, `I2C_AQ_NO_ZERO_LEN`, `SMBUS_QUICK` and
`copy_from_kernel_nofault()` fixes) is compiled **into** the trimmed kernel
rather than built as a module: `drivers/i2c/busses/pv-io.c` plus one `obj-y`
line. Built in because it is a `device_initcall` for a fixed-address bridge
that is always present on this machine, and building it in removes the
vermagic/`depmod` coupling entirely — a module built against the untrimmed
kernel would not load here anyway.

Cost: **+64 bytes** of Image (6,543,776 -> 6,543,840). `pv_io_init` and
`pv_i2c_xfer` are both in `vmlinux` and the initcall is registered.

Host verification, with the driver probing this machine's own bridge:

```
[    0.345938] ra8d1-pv: bridge v1, 0 i2c bus(es), 0 gpio(s), 256 B window
```

It finds the bridge, reads CAPS, does not oops, and the boot reaches a login
prompt. It reports **0 buses** because `host/platform_host.c` has no I2C — so
**the host harness cannot test the adapter at all**, and `/dev/i2c-0` only
appears on the board, where `plat_pv_caps()` reports 1 bus and 8 GPIOs. That
is a real limit of the harness and the reason the board test matters.

### The zero-length hardening, which is the failure mode worth knowing

The known trap is an I2C scan that reports **all 112 addresses** instead of
one. `emu/rv_machine.c` now rejects zero-length transfers explicitly:

```c
if ((cmd == PV_CMD_I2C_WRITE      && wlen == 0) ||
    (cmd == PV_CMD_I2C_READ       && rlen == 0) ||
    (cmd == PV_CMD_I2C_WRITE_READ && (wlen == 0 || rlen == 0))) {
        m->pv_status = (uint32_t)(-EINVAL);
        return;
}
```

A zero-length read handed to a Zephyr driver can return success without ever
putting the address on the wire, which the guest reads as an ACK from every
address. The guest driver's `I2C_AQ_NO_ZERO_LEN` quirk stops the kernel i2c
core generating one, but the bridge is MMIO and anything in the guest can write
these registers directly, so the check belongs on this side too.

On the related "stale `pv_i2c_wlen`" concern: **not a problem here, by
construction.** The length registers are deliberately not cleared between
commands — they are registers and the guest owns them — but each command reads
only the register it needs, and `pv_i2c_xfer()` writes that register
immediately before firing the command. A stale value from a previous transfer
cannot be picked up. Verified by reading both sides.

### Why the board test did not happen

The rootfs could not be written. After a controlled set of experiments the
fault is characterised, and it is **not** what I first guessed.

| size | slot | board state | result |
|---:|---|---|---|
| 3.3 MB | kernel | guest running | **OK**, repeatedly |
| 6.5 MB | kernel | guest running | **OK**, 4x incl. the pv-io kernel |
| 6.5 MB | **rootfs** | fresh reset | **OK** — 1m43s, verified |
| 24 MB | rootfs | fresh reset | FAIL, `ERR verify failed` |
| 24 MB | rootfs | after a failed push | FAIL, socket timeout |
| 37 MB | rootfs | various | FAIL x4 — timeout, no-ACK, verify x2 |
| 37 MB | rootfs | *earlier the same day* | **OK** — 9m32s, verified |

**It is not positional.** I hypothesised the rootfs slot's higher offsets were
the problem and that was wrong: 6.5 MB into the rootfs slot at 0x840000 writes
and verifies cleanly. The slot is fine.

**It correlates with transfer size**, failing somewhere between 6.5 MB and
24 MB — but it is not a hard limit either, because a 37 MB push verified
successfully earlier the same day. So it is marginal rather than deterministic,
and something about a long transfer makes it fail.

The dominant failure is `ERR verify failed`: the board receives the entire
payload, writes it, reads it back, and gets a different CRC. That is a write or
readback fault, not a transport fault.

**A failed push wedges the netload service.** After one, the next TCP push gets
`timed out after 15s waiting for the board (last partial: b'')` at the
handshake, and only a reset recovers it. That turns one failure into a run of
them and is worth fixing independently of the root cause.

**The TCP service is only reachable when rvlinux is running a guest.** In the
UART loader it accepts a connection but never progresses. Combined with an
invalid kernel slot — which sends rvlinux to the loader — that is a catch-22:
the transport that works needs a booted guest, and booting a guest needs the
transport.

**The UART loader's per-block ACK never arrived**, at 3.3 MB, 24 MB or 37 MB,
with and without `--no-prompt` and with `--ack-timeout` up to 300 s. The erase
completes and `<RDY>` arrives; the data phase then stalls.

Two of these are self-inflicted and worth separating out: putting this port's
MMU kernel in the kernel slot makes rvlinux enter it as a nommu M-mode guest,
which runs away and starves its own network thread; and a partial kernel-slot
write left the slot invalid. Neither explains the verify failures.

All of this is `rvlinux`'s loader and OSPI writer, not this port. Handed back.

### Board left, honestly

`rvlinux` is flashed and healthy — its paravirt selftest passes on every reset
and still finds the device at **0x14** on the real I2C bus, and it takes a DHCP
lease at 192.168.2.3.

**But the kernel slot is invalid**, from a partial write when a control push
was interrupted, so rvlinux prints `no valid kernel at 0x90040000 (rc 1)` and
sits in its UART loader. That means **no nommu guest and no telnet** until a
kernel is written back, and I could not write one: TCP needs a running guest
and UART never ACKs. This is worse than the state I was given, and it is the
one thing here I would undo if I could.

Recovering it needs either a working push path or a different route into the
flash. Nothing is lost — both images are on the Mac and the archived rvlinux
ELF reflashes in seconds — but it does need the loader bug fixed first.

---

## 9. What is NOT verified

Stated plainly, because the difference matters:

- **Never flashed. Never run on the board.** Another agent is using it. Every
  hardware claim here is inference from a clean build, not observation. This
  remains the single biggest gap: everything above ran on an arm64 Mac.
- **No throughput number for this board.** The MIPS figures here are macOS.
  The lead's 464–468 MIPS is also a desktop measurement. What an M85 at
  480 MHz does with a paging guest out of SDRAM is unmeasured.
- **Console input: confirmed on the board** (§8a). No longer a gap.
- **The OSPI read path: confirmed on the board** (§8a). No longer a gap.
- **This app still has no image writer.** It reads slots correctly on real
  hardware, but pushing needs the rvlinux app, which flashing this one
  replaced. Restoring it means reflashing the archived ELF.
- **64 MB is tight, and now measurably so.** The kernel alone reserves 26 MB:
  `Memory: 39204K/65536K available (7399K kernel code, 8860K rwdata, 4096K
  rodata, 4166K init, 327K bss, 26332K reserved)`. 39 MB for userspace and page
  cache is workable but it is not roomy, and it rules out a large initramfs.
- **The image loader and `tools/pushimage.py` have never moved a byte.** The
  container format is checked for self-consistency in Python against the
  layout `img_load()` reads, and the flash calls are the same ones the
  mini-rv32ima port uses, but the round trip has not been done.
- **The paravirt bridge is compiled, not exercised.** It was never exercised
  in the mini-rv32ima port either (`notes/05` §8), so this port inherits an
  untested feature rather than breaking a tested one.
- **Reboot is untested.** `SRST` type 1/2 sets the stop code and the app prints
  it, but nothing re-enters `rv_machine_init()` yet, so a guest reboot ends the
  run rather than restarting it.
- **The block device has never seen a write attempt.** `VIRTIO_BLK_F_RO` means
  a conforming guest never issues one, so the `VIRTIO_BLK_S_IOERR` path is
  reasoned rather than observed.
- **Only one virtqueue configuration has been exercised** — whatever Linux
  6.1's virtio-blk chose. The chain walker handles multi-segment chains and
  bounds them at 260 descriptors, but a driver that lays requests out
  differently has not been tried.

---

## 9a. The kernel-size blocker — SOLVED

It was: the raw MMU `Image` is 25.9 MB and the flash slot is 8 MB, and nothing
here decompresses.

**Fixed by trimming the kernel, not by adding a decompressor.** The trimmed
Image is **6,543,776 B — 3.96x smaller, and it fits the slot raw with 1.8 MB to
spare.** Full write-up, config and recipe in `guest/`.

Both changes were built and booted separately, so the attribution is measured
rather than apportioned:

| variant | Image | fits 8 MB? | RAM to guest | reserved |
|---|---:|---|---:|---:|
| A stock | 25,923,072 | no | 39,204K | 26,332K |
| B A + `STRICT_KERNEL_RWX=n` | 10,727,424 | **no** | 54,044K | 11,492K |
| C B + driver trim | **6,543,776** | **yes** | **58,216K** | **7,320K** |

The padding fix is the cheaper and larger single change — 58.6% of the Image
and +14.8 MB of guest RAM for one symbol — but it lands at 10.23 MB, still
2.3 MB over the slot. The two are not alternatives.

The finding worth carrying: **57% of the original Image was zeros.**
`CONFIG_STRICT_KERNEL_RWX` aligns `.text`, `.init.text`, `.rodata` and `.data`
to Sv32 4 MB megapage boundaries so each can carry its own permissions, which
on a kernel with 7.6 MB of `.text` puts the sections at 0, 8, 16 and 24 MB and
makes the Image the span of that. 14,855,859 bytes of padding out of
25,923,072. Config trimming took the real content from 11.07 MB to 6.78 MB on
top of that, but cutting 468 symbols *without* turning off STRICT_KERNEL_RWX
would still have left a ~21 MB Image.

Second benefit, which matters as much on this board: the guest gets **19 MB
more RAM**, because the kernel stopped reserving a quarter of the SDRAM.

```
before  Memory: 39204K/65536K available (... 26332K reserved)
after   Memory: 58216K/65536K available (...  7320K reserved)
```

Booted under the host harness with `rootfs.ext2` on virtio-blk to a login
prompt, `python3` 3.11.6 and `ctypes.CDLL(None)` returning a handle, with
`/sys/class/gpio` present. The header declares `text_offset = 0x400000`, so the
app's M-mode guard accepts it.

Not yet booted on the board. It now fits, which is what was in the way.

---

## 10. What remains

1. **Land a rootfs on the board again** (§8c). Blocks the `/dev/i2c-0` test and
   everything behind it. Belongs to whoever owns `rvlinux`'s loader.
2. **Test `i2cdetect -y -r 0`** once that is possible. The bridge selftest sees
   a device at **0x14** on the real bus, so a scan returning exactly `0x14`
   closes the Blinka path end to end. The eth0 fix (§8b, ~15 s of a 34.5 s
   boot) is built into the pending rootfs and lands with it.
2. **An image writer in this app.** Today it only reads slots; pushing needs
   the rvlinux app, which flashing this one replaced. Porting rvlinux's TCP
   pusher is the obvious answer and it is already tested code.
3. **Rebuild `ra8d1-linux/guest/pv-io.c`** against this kernel and see the
   paravirt bridge work for the first time. With Python and ctypes already
   proven in the guest, this is the last piece before a Blinka attempt.
4. **Try Blinka.** `import board` on an emulated RISC-V Linux driving real
   RA8D1 I2C through the bridge is the whole point of the project.
5. **A boot-to-shell measurement on the board**, to compare against the nommu
   port's 2.4 s. Expect much slower: this kernel is 25.9 MB against 3.3 and
   takes ~90 M instructions to reach userspace, plus 309 block requests to
   mount and log in.
6. **Decide the float ABI** (§6) — done for now, defaulting to 64; revisit only
   if the guest is ever rebuilt soft-float.
7. **EROFS**, if the smaller image and the read-only guarantee are wanted,
   bundled with the kernel trim in item 1.
8. **Make reboot actually reboot** (§9).
9. **Consider the telnet console.** It drops in at the `plat_putc`/`plat_getc`
   layer without touching `emu/`, which is what that layer is for.
10. **Upstream the `byteswap.h` and `riscv_cpu_end` findings** — the lead is
    handling this.

---

## 11. Progress log

| date | entry |
|---|---|
| 2026-08-07 | **Linux boots on the board.** Pushed the trimmed kernel and a rebuilt 36 MB rootfs into OSPI, reflashed, and rv32 Sv32 Linux 6.1.44 came up from flash to a `buildroot login:` prompt in **34.5 s from reset** — 5.66 s of that is kernel boot, matching the 15.37 MIPS prediction almost exactly, and 15 s is Buildroot waiting for an `eth0` that does not exist. `python3 -c "import ctypes"` works on the board. Block device does 3.4 MB/s in service, which is emulation-bound; the OSPI window itself does 61 MB/s. |
| 2026-08-07 | **Trimmed the guest kernel from 25.9 MB to 6.24 MB, so it fits the 8 MB flash slot raw — the hardware blocker is gone.** 57% of the original Image was alignment padding from `CONFIG_STRICT_KERNEL_RWX` putting Sv32 megapage-aligned sections at 0/8/16/24 MB, not features; config trimming (1178 -> 724 symbols, no PCI/USB/DRM/WLAN/NFS/RTC/debug) took the content from 11.07 to 6.78 MB on top. The guest also gains 19 MB of RAM. Boots to a login prompt with Python and ctypes working and `/sys/class/gpio` present. Built out-of-tree in `/br/mmu-trim`; recipe and config in `guest/`. |
| 2026-08-07 | **Flashed, and it runs on real silicon.** Every self-test check passes on the board: Sv32 aliasing, page-fault delegation, the `time` CSR, the SBI timer, DBCN. **15.37 MIPS with paging active** (293 MIPS for the same loop on the Mac, so the M85 is 19x slower), and **console input works** through the real UART. The OSPI window reads correctly with the D-cache on at 61 MB/s. First boot found a real bug that inspection had missed: the rvlinux nommu guest is a *valid* rv32 Image with a good CRC, so the Image-magic check could not reject it and the app hung entering an M-mode kernel in S-mode; `text_offset` (0 for M-mode, 0x400000 for rv32 otherwise) is the discriminator and the app now explains itself instead. No guest boots on the board yet: no kernel fits the 8 MB slot. |
| 2026-08-07 | **Booted the real Buildroot guest to a login prompt and ran Python.** Added a read-only virtio-mmio block device (1,576 B text) written rather than vendored, reading the rootfs in place from the flash window so it costs no SDRAM; `rootfs.ext2` mounts read-only as `/dev/vda` and `python3 -c "import ctypes"` works, which is the project's goal reached in emulation. Checked rather than assumed that the guest kernel has **no EROFS** — ext2 instead. Adopted rvlinux's tested flash slot geometry and header format and deleted this port's own container, loader and tools in favour of it. Console input settled: it works, and the earlier flakiness was input sent before the guest opened ttyS0. App is 89,172 B flash / 25,560 B RAM. Still not flashed; a kernel does not yet fit the 8 MB slot. |
| 2026-08-07 | **Real rv32 Sv32 Linux 6.1.44 boots to userspace** on this machine layer (host harness). Booting it found two machine-layer bugs — the initrd was placed inside the kernel's `.bss` because `image_size` > file size, and a bounded run could never end against a WFI-idle guest — both fixed, plus a configuration mismatch: the Buildroot userspace is ilp32d, so `FLEN` is now a build option defaulting to 64 (+14.5 KB flash, no measurable speed cost) and `riscv,isa` is derived from `misa` instead of hardcoded. The 8250 and PLIC are confirmed working under the real Linux drivers; 0 unsupported SBI calls across a full boot. OpenSBI recorded as the fallback in §3 with sizes and a switching recipe. Still not flashed. |
| 2026-08-07 | Port done to a building Zephyr app. TinyEMU RV32 core in at 8,652 B text / 6,400 B bss; whole app 74,164 B flash and 25,200 B RAM at FLEN=0. Own machine layer (RAM, 8250, CLINT, PLIC, syscon, paravirt bridge), runtime devicetree generator, and a host-C SBI instead of an OpenSBI blob. Six small patches to the vendored sources, all documented above; no dynamic allocation anywhere. Verified on the Mac against a purpose-written RV32 S-mode payload: Sv32 aliasing, page-fault delegation, the `time` CSR, and the SBI timer all check out, and `dtc` accepts the generated blob. Not flashed, and no Linux kernel booted — there is no rv32 MMU image yet. |
