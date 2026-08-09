# mini-rv32ima + RV32IMA Linux: integration contract, image, host validation

Date: 2026-08-07
Host used for validation: macOS 26.5.2, Apple Silicon (T8112 / M2), Apple clang 21.0.0.
Cross-compile size checks: `arm-none-eabi-gcc` 14.3.rel1 (homebrew `arm-gcc-bin@14`).

Everything below was actually run. Nothing is quoted from documentation without
being checked against the source.

---

## 0. What is on disk

```
ra8d1-linux/
  emulator/                      git clone of cnlohr/mini-rv32ima @ 84858f5
    mini-rv32ima/
      mini-rv32ima.h             17,167 B  <-- the reusable core, this is all we port
      mini-rv32ima.c             13,961 B  <-- host-only reference wrapper (POSIX/Win32)
      default64mbdtc.h            9,459 B  <-- sixtyfourmb.dtb as a C array (1536 B of data)
      sixtyfourmb.dts / .dtb                 the device tree the kernel is booted with
      mini-rv32ima                          built host binary (arm64 Mach-O, 52,448 B)
    cachetest/gpucache.h                     reference impl of MINIRV32_CUSTOM_MEMORY_BUS
    baremetal/                               tiny non-Linux test payload
  image/
    Image                      3,476,752 B  kernel + embedded initramfs, boots (PRIMARY)
    linux-6.1.14-rv32nommu-cnl-1.zip 1,507,463 B
    tiny/                                    secondary, NOT usable as-is (see §5)
  notes/
    01-emulator-and-image.md   this file
    host-boot.log              full 64 MB boot-to-shell capture
    host-boot-driver.py        pty harness used for every boot test
    cortex-m85-sizetest.c      minimal embedder TU used for the code-size numbers
    ramsweep/                  one log per RAM size tried
```

Commands that produced it:

```sh
cd /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux
git clone --depth 1 https://github.com/cnlohr/mini-rv32ima emulator

cd image
curl -sL -o linux-6.1.14-rv32nommu-cnl-1.zip \
  https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/linux-6.1.14-rv32nommu-cnl-1.zip
unzip linux-6.1.14-rv32nommu-cnl-1.zip     # -> Image

cd ../emulator/mini-rv32ima
cc -o mini-rv32ima mini-rv32ima.c -g -O2 -Wall   # clean, zero warnings
```

---

## 1. The integration contract

`mini-rv32ima.h` is an STB-style single header. You define macros, then
`#define MINIRV32_IMPLEMENTATION` and include it. It emits exactly one function:

```c
int32_t MiniRV32IMAStep( struct MiniRV32IMAState * state,
                         uint8_t * image,
                         uint32_t vProcAddress,   /* unused, pass 0 */
                         uint32_t elapsedUs,      /* microseconds since last call */
                         int count );             /* max guest instrs to run */
```

Return values, and what the embedder must do with them (from `mini-rv32ima.c:211`):

| ret | meaning | embedder action |
| --- | --- | --- |
| `0` | ran `count` instructions normally | loop again |
| `1` | guest is in WFI (halted, waiting for timer) | sleep, and add `count` to `state->cyclel` so guest time still advances |
| `3` | fatal fault, only if you set `fail_on_all_faults` | abort |
| `0x7777` | SYSCON reboot | reload the image and restart |
| `0x5555` | SYSCON poweroff | stop |

`0x7777` / `0x5555` come back out of `MINIRV32_HANDLE_MEM_STORE_CONTROL` via the
`return val;` in the reference macro — the store handler's return value becomes
the step function's return value. That is the mechanism, and it is easy to miss.

### 1a. Macros the embedder defines

All are optional except `MINI_RV32_RAM_SIZE` and `MINIRV32_IMPLEMENTATION`;
every other one has a no-op default at the top of the header (lines 24-69).

| Macro | Signature / value | Meaning |
| --- | --- | --- |
| `MINIRV32_IMPLEMENTATION` | (defined) | emit the function body, not just the prototype |
| `MINI_RV32_RAM_SIZE` | `uint32_t` expression | size of guest RAM in bytes. Can be a variable (the reference uses the global `ram_amt`) or a compile-time constant. Making it a constant is measurably better for us — see §3. |
| `MINIRV32_RAM_IMAGE_OFFSET` | default `0x80000000` | guest physical base of RAM. All guest addresses have this subtracted before indexing `image`. Do not change it; the kernel and DTB are built for `0x80000000`. |
| `MINIRV32_MMIO_RANGE(n)` | default `(0x10000000 <= (n) && (n) < 0x12000000)` | which guest addresses are MMIO rather than RAM |
| `MINIRV32_DECORATE` | default `static` | storage class on `MiniRV32IMAStep` |
| `MINIRV32WARN(x...)` | default no-op | warning output; safe to leave as no-op on the MCU |
| `MINIRV32_POSTEXEC(pc, ir, retval)` | statement | runs after every guest instruction *and* on every trap. This is the hot path — keep it empty or trivially cheap. |
| `MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val)` | statement | MMIO write. `addy` is the full guest address (0x1xxxxxxx). To signal poweroff/reboot up to the caller the macro must expand to something containing `return val;`. |
| `MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval)` | statement, assigns `rval` | MMIO read |
| `MINIRV32_OTHERCSR_WRITE(csrno, value)` | statement | CSR write not handled internally |
| `MINIRV32_OTHERCSR_READ(csrno, value)` | statement, assigns `value` | CSR read not handled internally |
| `MINIRV32_CUSTOM_MEMORY_BUS` | (defined) | opt out of the flat-array accessors, supply your own (§2) |
| `CUSTOM_MULH` | `case` labels | replace the 3 MULH variants if you have no 64-bit multiply. **We do not need this on Cortex-M85** — see §3. |
| `MINIRV32_STEPPROTO` | full prototype | change the function signature (e.g. drop args) |
| `MINIRV32_CUSTOM_INTERNALS` | (defined) | replace the `CSR()/SETCSR()/REG()/REGSET()` accessors |

### 1b. Processor state

```c
struct MiniRV32IMAState {   /* sizeof == 192 bytes, verified by compiling it */
    uint32_t regs[32];
    uint32_t pc, mstatus, cyclel, cycleh;
    uint32_t timerl, timerh, timermatchl, timermatchh;
    uint32_t mscratch, mtvec, mie, mip;
    uint32_t mepc, mtval, mcause;
    uint32_t extraflags;    /* bits 0-1 priv, bit 2 WFI, bits 3+ LR/SC reservation */
};
```

192 bytes. This can live in fast internal SRAM on the RA8D1 — it does not have
to be inside the emulated RAM. The reference wrapper puts it at the very top of
the RAM buffer only as a convenience.

### 1c. What the host must provide

Three things, and that is genuinely all:

1. **A microsecond time source.** You pass `elapsedUs` into every `MiniRV32IMAStep`
   call; the header adds it to the 64-bit `timerl/timerh` and fires the machine
   timer interrupt when it passes `timermatchl/h`. The DTB declares
   `timebase-frequency = <0xf4240>` = 1 MHz, so the unit really is microseconds.
   The header never reads a clock itself.
2. **UART in/out**, wired through `MINIRV32_HANDLE_MEM_STORE_CONTROL` /
   `..._LOAD_CONTROL`. The kernel drives a stock 8250/16550 at `0x10000000`:
   - store to `0x10000000` -> transmit that byte
   - load `0x10000005` -> Line Status Register; reference returns `0x60 | IsKBHit()`
     (0x60 = THR empty + transmitter empty, bit 0 = RX data available)
   - load `0x10000000` -> next RX byte
   That is the entire UART model. Two addresses.
3. **CLINT registers**, same two macros:
   - store `0x11004000` / `0x11004004` -> `timermatchl` / `timermatchh` (mtimecmp)
   - load `0x1100bff8` / `0x1100bffc` -> `timerl` / `timerh` (mtime)
   - store `0x11100000` -> SYSCON: return the written value so the caller sees
     `0x5555` (poweroff) or `0x7777` (reboot)

### 1d. Freestanding?

**The header is genuinely freestanding.** It contains no `#include` at all, calls
no function, allocates nothing, and touches no file. It needs only the fixed-width
types (`uint32_t`, `int32_t`, `int64_t`) in scope before inclusion.

Verified empirically: I compiled a minimal embedder TU (`notes/cortex-m85-sizetest.c`)
that defines the macros, includes the header, and provides the UART/CLINT handlers.
`arm-none-eabi-nm` on the resulting object shows only three undefined symbols, all
of them mine:

```
U getc_uart
U putc_uart
U ram_amt
```

Zero libgcc references. Notably `__aeabi_lmul` is **not** pulled in: the `MULH`
family's `int64_t` multiply compiles to a single `SMULL`/`UMULL`/`SMLAL` on ARMv7-M
and up, so `CUSTOM_MULH` is unnecessary for us.

**`mini-rv32ima.c` is host-only** and is a template, not a dependency. It contains:
argv parsing, `malloc`, `fopen`/`fread` of the kernel and DTB, `termios` raw-mode
keyboard capture, `ioctl(FIONREAD)`, `gettimeofday`, `usleep`, `printf`, a register
dumper, and `#include <math.h>` (which is not actually used). None of that ports.
What we reuse from it is the ~40 lines of `HandleControlStore`/`HandleControlLoad`
and the boot-time register setup in §4.

---

## 2. Memory access — flat array *or* full callback

By default (header lines 60-69) memory is a **flat little-endian byte array**:

```c
#define MINIRV32_STORE4( ofs, val ) *(uint32_t*)(image + ofs) = val
#define MINIRV32_LOAD4( ofs )       *(uint32_t*)(image + ofs)
/* ... STORE2/STORE1/LOAD2/LOAD1/LOAD2_SIGNED/LOAD1_SIGNED likewise */
```

`ofs` is already RAM-relative (guest address minus `0x80000000`) and already
range-checked by the header before the macro is invoked.

Defining `MINIRV32_CUSTOM_MEMORY_BUS` suppresses all eight and you supply them
yourself, as macros *or* as real functions. `cachetest/gpucache.h:195-203` is the
worked example — it routes every access through a software cache:

```c
#define MINIRV32_CUSTOM_MEMORY_BUS
uint MINIRV32_LOAD4( uint ofs ) { return LoadMemInternal( ofs, 4 ); }
#define MINIRV32_STORE4( ofs, val ) { StoreMemInternal( ofs, val, 4 ); ... }
int  MINIRV32_LOAD2_SIGNED( uint ofs ) { uint t = LoadMemInternal(ofs,2); if (t & 0x8000) t |= 0xffff0000; return t; }
/* etc. */
```

**So yes, every single access can be routed through a callback.** This is exactly
the hook we would use if we wanted SDRAM behind a cache, or wanted to page from
octo-SPI flash. Two things to be aware of before we reach for it:

- `MINIRV32_LOAD4` is also the **instruction fetch** path (header line 172). It is
  the single hottest operation in the emulator; anything more than a load there
  costs on every guest instruction.
- The signed loads must sign-extend themselves — the header does not do it for you.

For the RA8D1 the flat-array default is the right starting point: 64 MB of
external SDRAM is directly addressable, so `image` is just a pointer into it and
the accessors stay single instructions. The callback path is our fallback if
SDRAM turns out too slow and we want a small SRAM-resident cache in front of it.

### Alignment and endianness

- **Little-endian is required.** Cortex-M85 is LE by default. Fine.
- **The default accessors do unaligned accesses.** RISC-V permits misaligned
  loads/stores and the header does not trap or fix them up — it hands the raw
  offset to a `*(uint32_t*)` cast. Cortex-M85 supports unaligned `LDR`/`STR`, but
  **only to memory typed Normal**. If we map the SDRAM region as Device memory in
  the MPU, an unaligned guest access becomes a hard fault. Map SDRAM as
  Normal, cacheable, and leave `SCB->CCR.UNALIGN_TRP` clear.
- The `image` base pointer itself should be at least 4-byte aligned; align it to
  32 bytes so guest word accesses never straddle a cache line unnecessarily.

---

## 3. Code size on Cortex-M85

`notes/cortex-m85-sizetest.c` = the header plus the UART/CLINT/SYSCON handlers,
i.e. everything that actually has to run on the MCU. Compiled with
`arm-none-eabi-gcc -mcpu=cortex-m85 -mthumb -mfloat-abi=hard`:

| flags | .text | .bss |
| --- | --- | --- |
| `-Os` | **2,316 B** | 4 |
| `-O2` | 3,028 B | 4 |
| `-O3` | 3,032 B | 4 |

Under 3 KB of flash for the whole CPU core. Against 2 MB of internal flash this
is free; the interesting budget is entirely RAM, not code.

Command:
```sh
arm-none-eabi-gcc -c notes/cortex-m85-sizetest.c -Iemulator/mini-rv32ima \
  -mcpu=cortex-m85 -mthumb -mfloat-abi=hard -Os -o st.o && arm-none-eabi-size st.o
```

One tuning note: the reference makes `MINI_RV32_RAM_SIZE` a *variable* (`ram_amt`),
which forces a memory load on every bounds check. Making it a compile-time
constant on our port turns each of those into an immediate compare. Worth doing.

---

## 4. Boot / memory layout the emulator sets up

From `mini-rv32ima.c:109-191`. This is the contract our RA8D1 startup code must
reproduce:

```
guest phys 0x80000000  ->  image[0]
```

| what | where |
| --- | --- |
| kernel `Image` | loaded flat at `image[0]`, i.e. guest `0x80000000`. It is a raw blob, not ELF, not `vmlinux`, not a `zImage` — no header parsing, just `memcpy`. |
| rest of RAM | zeroed (`memset(ram_image, 0, ram_amt)`) before the image is copied |
| DTB (1,536 B) | `dtb_ptr = ram_amt - 1536 - 192`, i.e. just under the state struct |
| `MiniRV32IMAState` (192 B) | the last 192 bytes of RAM |

Initial CPU state:

```c
core->pc         = 0x80000000;
core->regs[10]   = 0;                        /* a0 = hart id            */
core->regs[11]   = dtb_ptr + 0x80000000;     /* a1 = physical DTB ptr   */
core->extraflags |= 3;                       /* start in machine mode   */
```

Two patches applied to the built-in DTB, both verified by compiling against
`default64mbdtc.h`:

- **RAM size**, at byte offset `0x13c` of the DTB. It holds the sentinel
  `0x00c0ff03` (big-endian `0x03ffc000`, the 64 MB-ish size in `sixtyfourmb.dts`);
  the wrapper byte-swaps `dtb_ptr` into it, so the kernel is told that usable RAM
  ends where the DTB begins. If we change `MINI_RV32_RAM_SIZE` we **must** apply
  this patch or the kernel will walk off the end of real memory.
- **Kernel command line**, at byte offset `0xc0`, max 54 chars. Default value:
  `earlycon=uart8250,mmio,0x10000000,1000000 console=ttyS0`

Device tree (`sixtyfourmb.dts`), for reference: `compatible = "riscv-minimal-nommu"`,
one rv32ima hart with `mmu-type = "riscv,none"`, `ns16850` UART at `0x10000000`,
syscon at `0x11100000`, CLINT at `0x11000000`, `timebase-frequency` 1 MHz.

---

## 5. The image

### Primary — WORKS

| | |
| --- | --- |
| file | `image/Image` |
| size | **3,476,752 bytes** |
| sha256 | `5f596134705d5aa8e7c8c406695a560d08faaff4a86bc715659e53cbebba6c7e` |
| source | https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/linux-6.1.14-rv32nommu-cnl-1.zip |
| zip sha256 | `add651195348b538c309becb39c5f8ef4f9d15ec275a2954b02016fc38091393` |
| format | raw flat RISC-V kernel image (`Image`), **initramfs embedded inside it** |
| kernel | Linux 6.1.14, riscv32 nommu, Buildroot userspace, busybox |
| DTB | **not** in the file — supplied separately by the emulator (built into `default64mbdtc.h`) |
| load address | `0x80000000`, i.e. offset 0 of guest RAM |
| rootfs | none needed. No block device, no SD card, no filesystem driver. |

One file, one `memcpy`, no storage stack. For an MCU port this is the ideal shape.

### Secondary — downloaded, does NOT work with stock mini-rv32ima

`image/tiny/` from https://github.com/tvlad1234/buildroot-tiny-rv32ima releases
`v1.0` (`images.zip`, 2,433,614 B). This is what `pico-rv32ima` (RP2040) uses.

```
Image     2,203,460 B   raw kernel
dtb           2,048 B   device tree, separate file
rootfs   62,914,560 B   ext2, 60 MB, needs a block device
stat              2 B
```

I tried booting it under the stock emulator (`-m 0x1000000`): **zero bytes of
output in 25 s**. Reason, from decompiling its `dtb`: it has *no* `uart@10000000`
node. It declares only syscon, clint, and `tvlad1234,spi-tiny-rv32ima`. Console
and rootfs both go through pico-rv32ima's own custom MMIO/CSR devices, and the
60 MB ext2 needs a block driver on the emulator side.

Keeping it as a reference for how a block-device-backed rootfs is wired, but it
is **not** a drop-in and I would not build the RA8D1 port around it. pico-rv32ima
runs 8 MB of emulated RAM in external SPI PSRAM, which is a useful data point
for our budget but not otherwise relevant given we have 64 MB of SDRAM.

Buildroot was not needed and was not started.

---

## 6. Host boot — it works

Harness: `notes/host-boot-driver.py`, which runs the emulator under a pty (a pipe
is not enough — the reference `IsKBHit()` calls `write(fileno(stdin),0,0)` and
latches EOF on a read-only pipe), waits for `buildroot login:`, sends `root`,
waits for the shell prompt, runs a command, then powers off.

```sh
cd /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux
python3 notes/host-boot-driver.py \
  emulator/mini-rv32ima/mini-rv32ima image/Image 0x4000000 notes/host-boot.log 120
```

Result at 64 MB:

```
login_prompt   0.31s
shell_prompt   0.79s
cmd_done       1.33s
poweroff       1.76s
```

**Yes, it reaches a shell.** Full log in `notes/host-boot.log`. The interesting end:

```
[    0.084015] Freeing unused kernel image (initmem) memory: 1464K
[    0.084109] This architecture does not have kernel memory protection.
[    0.084203] Run /init as init process

Welcome to Buildroot
buildroot login: root
~ # uname -a; free
Linux buildroot 6.1.14 #4 Sat Mar 25 09:20:08 EDT 2023 riscv32 GNU/Linux
              total        used        free      shared  buff/cache   available
Mem:          62848        2892       57584           0        2372       56212
~ # [    1.707706] reboot: Power down
```

Login is `root`, no password.

**Timing: 0.79 s wall clock from launch to shell prompt** on an M2. Guest-reported
kernel time to `Run /init` is 0.084 s (the guest's 1 MHz timer is driven from real
elapsed microseconds, so guest time runs much faster than wall time here).

### Throughput, for extrapolating to the RA8D1

Read from the `POWEROFF@` cycle counter at exit:

```
guest instructions retired to boot + poweroff: 48,703,918
wall time: 1.09 s
=> ~44.6 M guest instructions/sec on this Mac
```

So a full boot to shell is roughly **35-40 million guest instructions**. That is
the number to divide by whatever the M85 achieves. I have *not* measured the M85
side, so I am not going to guess a boot time.

---

## 7. Minimum `MINI_RV32_RAM_SIZE` that boots

Same harness, same image, `-m <size>`. Logs in `notes/ramsweep/`.

| RAM | `-m` | login prompt | shell prompt | ran `uname -a; free` | verdict |
| --- | --- | --- | --- | --- | --- |
| 64 MB | `0x4000000` | 0.31 s | 0.79 s | yes | **PASS** |
| 32 MB | `0x2000000` | 0.28 s | 0.76 s | yes | **PASS** |
| 16 MB | `0x1000000` | 0.28 s | 0.76 s | yes | **PASS** |
| 12 MB | `0xC00000` | 0.30 s | 0.78 s | yes | **PASS** |
| 11 MB | `0xB00000` | 0.27-0.29 s | 0.75-0.77 s | yes | **PASS** (3/3 runs) |
| 10 MB | `0xA00000` | 0.27 s | 0.76 s | **no** | **FAIL** |
| 9 MB | `0x900000` | — | — | — | **FAIL** |
| 8 MB | `0x800000` | — | — | — | **FAIL** |

**Minimum that boots to a usable shell: 11 MB (0xB00000).** Repeated three times,
identical every run.

The failures are worth reading precisely, because 10 MB is a trap:

- **10 MB** gets all the way to a login prompt and echoes your commands, then
  cannot exec anything:
  ```
  [    1.182257] binfmt_flat: Unable to allocate RAM for process text/data, errno -12
  uname -a; free
  Segmentation fault
  Segmentation fault
  ```
  It *looks* like it booted. It did not. Anything that only checks for a prompt
  will report a false pass here.
- **9 MB** panics outright: `Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b`
- **8 MB** OOMs during init, before login.

Why the cliff is so sharp: this is a **nommu** kernel, so every process needs a
physically *contiguous* block for its text+data. It is not total free memory that
runs out, it is contiguity. At 10 MB the failing allocation happens with 3,432 kB
still free but the largest free block only 512 kB:

```
Normal free:3432kB ... managed:8004kB
Normal: 4*4kB 5*8kB 1*16kB 3*32kB 3*64kB 2*128kB 3*256kB 4*512kB 0*1024kB 0*2048kB 0*4096kB = 3432kB
```

Practical reading for the RA8D1: **11 MB is the floor, and it is a cliff, not a
slope.** Sizing to the floor means every additional process is a coin flip against
fragmentation. With 64 MB of SDRAM on the board there is no reason to go near it
— run 32 MB or 64 MB. The floor matters only as evidence that if SDRAM bring-up
gives us trouble, a reduced-size configuration is still viable down to ~11 MB, and
that the 8 MB figure pico-rv32ima advertises is **not** achievable with this image
(they use a much smaller purpose-built userspace).

---

## 8. Flagged / not verified

- **Not measured: emulation speed on the M85 itself.** Everything here is host
  validation. The ~40 M guest instructions per boot is the number to carry
  forward, but instructions-per-guest-instruction on Cortex-M85 with SDRAM
  latency is unknown until we run it. I deliberately have not estimated a boot
  time from it.
- **Unaligned access is a real risk, not a theoretical one.** The default
  accessors will issue unaligned 32-bit loads. This works on the Mac and will
  work on the M85 only if SDRAM is mapped Normal in the MPU. Worth checking early
  in bring-up rather than debugging a hard fault later.
- **`image/tiny/` does not boot** under the stock emulator, as described in §5.
  That is expected and explained, not an unexplained failure.
- **No RA8D1 code was written and nothing was built for the target.** Only a
  size-check object file was cross-compiled, and it was deleted.
- Buildroot was not run. It was not needed — option 1 in the preference order
  worked on the first try.
