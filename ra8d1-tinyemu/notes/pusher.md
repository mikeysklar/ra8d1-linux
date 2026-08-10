# Porting the TCP image pusher into the TinyEMU app

2026-08-09. Build only — nothing here has touched hardware.

The rvlinux app on this board serves an image loader on TCP 5555 and the
TinyEMU app did not, so replacing a guest image meant flashing rvlinux,
pushing, and flashing back. That is BUILD.md gotcha 14 performed on purpose,
twice per iteration, with a known-good ELF as the only thing standing between
the operator and a rebuild.

This port makes the TinyEMU app serve the same protocol on the same port, so
`ra8d1-linux/rvlinux/tools/pushimage.py` drives it unchanged.

## The problem rvlinux does not have

rvlinux copies its kernel out of OSPI into SDRAM at boot and never reads the
window again, so a push lands underneath a running guest and takes effect at
the next reset. The comment in `rvlinux/src/main.c` says, in as many words,
that this stops holding the moment the guest reads flash directly.

This app is that moment. virtio-blk hands the rootfs slot to the guest as a
pointer into the memory-mapped OSPI window and the guest reads it in place for
its whole life — which is the only reason a 55 MB rootfs fits on a board with
64 MB of SDRAM. An OSPI-B device cannot serve memory-mapped reads while a
program or erase is in progress, and the mapped window has no way to report a
fault, so a push racing a live guest corrupts every read that collides with it,
silently. Erasing the slot the guest is running from is worse again.

Four options, three rejected:

| option | why not |
|---|---|
| let the push race the guest | the silent corruption above |
| A/B slots, switch at reset | needs twice the flash; the rootfs slot is already 55.75 MB of a 64 MB part |
| stage to SDRAM, commit at reset | needs 36 MB of RAM the guest is using |
| **stop the guest, push, reboot** | **freezes the guest for the push, which is minutes — and a push is followed by a reboot anyway** |

The freeze is not really a cost. The whole point of a push is to replace the
image the guest is running, so the guest's remaining lifetime after a push
begins is zero either way.

**No new emulator code was needed.** `rv_request_poweroff()` already ends the
run loop at the next instruction-slice boundary from another thread — it is how
the guest's own syscon poweroff works — so the pusher calls it and waits, and
`main()`'s loop is untouched. A slice is 10,000 instructions, ~667 us at the
measured 15.37 MIPS, against a 5 s bound.

Two details that are not obvious:

- **The request is repeated every 100 ms, not issued once.** `main()` runs the
  image CRCs (seconds, at rootfs sizes) *before* `rv_machine_init()`, and
  `rv_machine_init()` memsets the machine — including the stop flag — on its
  way past. A push arriving during the CRC would have its single request erased
  and then wait out the timeout against a guest that runs happily forever.
- **`main()` reports the halt to the pusher on every exit path**, including
  `rv_machine_init()` failing. Without that, a board whose image will not
  initialise could never get its halt acknowledged and could never be
  re-imaged — which is precisely the board that needs a pusher most.

The rootfs is mounted read-only (the block device advertises
`VIRTIO_BLK_F_RO` and rejects writes), so stopping the guest mid-instruction
cannot corrupt a filesystem. There is nothing to sync.

## Why the reboot is deferred

Normal operation is two pushes back to back — kernel, then rootfs. Rebooting
after the first would boot a new kernel against the old rootfs and then make
the operator wait out DHCP before the second push could start.

So a *successful* push arms a reboot 30 s out
(`CONFIG_RVT_PUSHER_REBOOT_DELAY_S`) and every new connection defers it again.
The board reboots once the pushing stops. The listener waits for clients with
`zsock_poll()` on the listening socket rather than blocking in `accept()`,
which is what makes the deadline reachable; `accept_q` is a union alias for
`recv_q` in Zephyr's socket layer, so `POLLIN` on a listener does signal a
pending connection.

A *failed* push deliberately does not arm it. `pushimage.py` retries three
times by default and a board that rebooted between attempts would refuse every
one of them with a connection error. The guest is already stopped and the slot
is already erased, so staying up costs nothing and is what makes the retry
work.

The two remaining cases:

- **Flash writer wedged** (`STALLED`, the FSP unbounded-spin case): the OSPI
  controller only comes back with a reset, the host is told `reset the board`,
  which `pushimage.py` treats as fatal and stops retrying — and then the board
  resets itself. rvlinux refuses further pushes until someone does it by hand.
- **`REBOOT_DELAY_S=0`**: no automatic reboot at all; the board sits with a
  stopped guest and says so.

## What was ported and what is new

Ported essentially verbatim from `rvlinux/src/main.c`, including every loader
fix that path gained on 2026-08-08:

- the wire protocol and the `RA8LDR 1 name:off:cap` banner
- `img_accept_len` / `img_erase` / `img_commit` ordering — bound the length
  before erasing, payload first, header last
- the blank check after erase (`ERASE INCOMPLETE`), which separates an
  incomplete erase from a bad write; NOR programming only clears bits, so
  writing over a marginally erased block yields the AND of old and new, which
  is stable and wrong
- verify-on-attempt with 3 attempts, and the line that reports which attempt
  succeeded — the attempt count is the finding, not the pass/fail
- `img_diagnose`: erased-block scan, and a two-pass CRC with a cache
  invalidate between, to tell a stale read from bad data
- `BLKCRC`/`ENDCRC` per-64 KB streaming so the host can diff against the source
  it still holds
- the double-buffered writer thread, its stall detection, and the
  abandoned-writer latch
- `zsock_poll()`-based receive timeouts rather than trusting `SO_RCVTIMEO`

New here:

- `halt_guest()` and the `main()` handshake (above)
- the deferred reboot and its poll-based accept loop
- `src/image.h`, so the on-flash container is defined once for the reader
  (`main.c`) and the writer (`pusher.c`) rather than twice
- `plat_flash_dev()` / `plat_slot_offset()` / `plat_flash_erase_size()` in
  `platform_zephyr.c`, keeping board knowledge out of `pusher.c` the same way
  `plat_slot_base()` already does for reads
- **the erase reserve**, below, which is a real difference from rvlinux and not
  a stylistic one

## The erase reserve, which this app's devicetree gets wrong for writing

Zephyr's `flash_erase()` asks the page layout for the page at `offset + len`,
and `flash_get_page_info()` returns `-EINVAL` one past the final page, so an
erase ending exactly at the top of the chip fails outright. rvlinux keeps the
last 256 KB block in hand for this reason and its rootfs slot stops at
0x3FC0000.

This app's `rvl_rootfs_partition` runs to 0x4000000 — the very top — because
the partition was only ever *read* through the memory-mapped window, and reads
do not care. Nothing was wrong with that until now.

So `plat_slot_offset()` applies the reserve itself and reports a writable size
256 KB smaller than `DT_REG_SIZE` for the rootfs. Without it the banner would
advertise capacity the erase cannot reach and a full-size rootfs push would die
on its last block after twelve minutes of work.

The numbers land exactly where `pushimage.py` expects, which is the check that
matters:

| slot | offset | board advertises | pushimage.py's own constant |
|---|---|---|---|
| kernel | 0x040000 | 8,384,512 | 8,384,512 |
| rootfs | 0x840000 | 58,191,872 | 58,191,872 |

The kernel also carries a RAM ceiling — `rv_machine_init()` copies it to the
base of guest RAM and puts the devicetree in the last megabyte — but at 8 MB of
slot against 64 MB of SDRAM the flash bound always binds first. It is coded
anyway so that shrinking the SDRAM cannot quietly turn a push into an
overwritten devicetree. It is necessary but not sufficient: the kernel's BSS
extends past the image and no length on the wire can predict it.
`rv_machine_init()` rejects that case with `-ENOMEM`.

## The 16-byte ECC unit, and the retry that would have been a bug

Added 2026-08-09 after the coordinator's research on the part.

The S28HL512T is Semper NOR with internal ECC computed over **16-byte data
units**, and the datasheet prohibits programming a unit twice without an erase
in between. The syndrome is written when the unit is programmed; programming it
again leaves the syndrome inconsistent with the data, and the part answers with
either a Program Error or — worse — data that is present, stable across every
read, and wrong. Infineon documents the rule, and the U-Boot and Linux MTD
drivers for the S28 series enforce it.

**Suspected, not proven: this is the best candidate yet for BUILD.md's
intermittent verify failure.** The signature matches exactly — a 24 MB push
whose payload was written, read back stably across two passes, and was wrong.
That is what a corrupted ECC unit looks like and it is not what a marginal
read looks like. It is written here as suspicion with a citation, because
nothing has been measured on the board.

What this port does about it, all enforced in code rather than left to care:

1. **Nothing re-programs, ever.** `CONFIG_RVT_PUSHER_VERIFY_ATTEMPTS` was
   already a *read* retry — it re-reads and re-CRCs the same bytes with a cache
   invalidate between, and never rewrites them. A push that exhausts its
   attempts fails the whole push and `pushimage.py` re-pushes, which erases
   first. That was luck rather than design, so the invariant is now stated at
   the top of `pusher.c` in capitals, and repeated at the verify loop and in
   the Kconfig help, specifically so that nobody adds an innocent-looking
   rewrite-on-failure later.
2. **Header and payload share no unit.** The header write covers
   `[0, IMG_PAYLOAD_OFF)` and the payload starts at `IMG_PAYLOAD_OFF` = 4096.
   `BUILD_ASSERT(IMG_PAYLOAD_OFF % 16 == 0)`.
3. **Every payload program starts and ends on a unit boundary.** Starts:
   4096 + a multiple of the buffer size. Ends: full-buffer writes by
   construction, guarded by `BUILD_ASSERT(CONFIG_RVT_PUSHER_BUF % 16 == 0)` —
   a buffer size that was not a multiple of 16 would make *every* write
   reprogram the unit the previous one left half done, which is the violation
   at its worst. The final short chunk is padded up to 16 with 0xFF in
   `nl_push()`.

Both asserts were negative-tested: building with
`-DCONFIG_RVT_PUSHER_BUF=8190` fails with
`static assertion failed: "every full-buffer write must end on a 16-byte ECC
unit..."`. They are not vacuous.

The padding is belt and braces rather than a fix: nothing programs past the
payload, so the half-filled tail unit would never be touched a second time
anyway. It is there so that stays true by construction instead of by argument.
It costs at most 15 bytes past the image, into a region that was erased anyway
and that the verify does not read.

Related and unfixed: the `PAGE_SIZE_BYTE=64` hardcode
(mikeysklar/zephyr#11) has no upstream fix, so write throughput stays 4-8x
below the part's capability. Do not model push durations on anything better —
`pushimage.py`'s 811 us per 64-byte page is the number that has been measured.

## pushimage.py compatibility

The tool is unmodified and unmodifiable by this port. Audit trail, board side
against `tools/pushimage.py`:

| tool expects | board sends / accepts |
|---|---|
| banner `RA8LDR 1 <name>:<off>:<cap> ...` | `nl_send_banner()`, same `snprintk` format |
| protocol version `1` | `1` |
| slot index = position in the banner | `PLAT_SLOT_KERNEL=0`, `PLAT_SLOT_ROOTFS=1`, iterated in order |
| `kernel` at 0x40000, `rootfs` at 0x840000 | `plat_slot_offset()`, BUILD_ASSERTed in `platform_zephyr.c` |
| header `'S'`, slot u8, len u32le, crc32 u32le | `nl_push()` decodes exactly that |
| `OK header`, then `OK erased`, then payload | same order, same strings |
| no per-block ACK during the payload | none sent |
| `OK committed`, or `ERR ...` | same |
| `BLKCRC <bs> <n>` / hex CRCs / `ENDCRC` before a verify error | `nl_send_blkcrc()` |
| `reset the board` / `disabled` in an error line means fatal | both phrasings kept |
| header at slot base, payload at +4096, magic `RA8LINUX` / `RA8ROOTF` | `src/image.h` |

The UART fallback loader (`'L'` at the boot prompt) was **not** ported. It
exists in rvlinux as the path that works before the network is up; here the
same recovery is a `probe-rs` reflash of a known-good ELF, which this project
already keeps and which is 20 seconds. Adding a second transport for a case
that already has an answer is code that would only ever be exercised by
accident. `pushimage.py --tcp` is unaffected.

## Build

`west build -p always -b ek_ra8d1`, both configurations clean, no new compiler
warnings.

| | text | data | bss |
|---|---:|---:|---:|
| telnet build (before) | 167,992 | 5,380 | 74,073 |
| **with the pusher** | **176,708** | **5,460** | **100,210** |
| delta | +8,716 | +80 | +26,137 |
| `-DCONFIG_NETWORKING=n` | 90,568 | 2,060 | 22,874 |

Flash is 8.82% of 2016 KB, RAM 11.71% of 896 KB. The bss is nearly all
buffers, and it is a deliberate choice rather than a surprise: 16 KB of double
buffer (`CONFIG_RVT_PUSHER_BUF` x2), 4 KB for the header block, 3 KB listener
stack, 2 KB writer stack. Internal SRAM only — the guest's 64 MB is external
SDRAM and none of this competes with it.

`-DCONFIG_NETWORKING=n` still builds. It emits the same pile of Kconfig
"assigned but got" warnings it emitted before, for the net symbols `prj.conf`
sets unconditionally; this port adds one more to that pile
(`NET_CONTEXT_RCVTIMEO`).

## Not tested

**Nothing here has run on hardware.** No board was touched, no `/dev/cu.*`, no
probe-rs. Everything below is a build-time and read-time argument.

In particular these are untested: the guest actually stopping on request, the
halt handshake's timing, the erase/write path in this app's driver
configuration, `sys_reboot()` bringing the board back with the OSPI controller
and the SDRAM re-initialised, and the deferred reboot's interaction with a real
`pushimage.py` retry.

## Risks for the hardware test

1. **The halt is the new thing; watch for `guest did NOT stop`.** If it appears,
   nothing has been erased — the check is deliberately before the erase — so
   the failure is safe, and the cause is either the emulator not reaching a
   slice boundary or a thread-priority inversion. Both are visible on the
   console.
2. **`sys_reboot(SYS_REBOOT_COLD)` on this SoC is unexercised in this project.**
   Everything so far has been reset by probe-rs. If a software reset comes back
   with the OSPI controller in a state the driver's init does not recover, the
   symptom will be a board that reboots into a kernel it cannot read. Recovery
   is a probe-rs reset, which is known to work. Worth testing `REBOOT_DELAY_S`
   with a small kernel push before trusting it with a 36 MB rootfs.
   Note that a *software* reset does not re-enumerate the J-Link OB VCOM, so
   BUILD.md gotcha 21 does not apply to it: the serial console should survive.
3. **The push fault itself is not fixed and was never claimed to be.** ~25% per
   attempt at 24 MB on the rvlinux app. The blank check and `BLKCRC` are here to
   catch it, not to prevent it, and `--retries 3` still does the landing. If it
   turns out to be materially better or worse under this app that is itself the
   finding — this app halts the guest first, so the OSPI controller is doing
   nothing else during the push, which rvlinux could not say.

   The ECC-unit rule above is the leading hypothesis for it, but note that this
   code did not violate that rule before the change either, so **the padding and
   the asserts are not expected to change the failure rate.** If they appear to,
   suspect the measurement: the rate was scored at ~25% per attempt on a sample
   small enough that a couple of runs prove nothing. What would move the needle
   is finding a *second* program to a unit somewhere else on the path — the RA
   OSPI-B driver's own 64-byte page handling is the place to look, since it is
   already known to be wrong about the page size (mikeysklar/zephyr#11).
4. **BUILD.md gotcha 19 — reset before pushing.** The rvlinux pusher degraded
   with app uptime: six consecutive failures on a 12-hour-old app, zero after a
   reset. Reset first here too, and treat any measurement taken on a
   long-running app as void.
5. **Console interleaving before the halt.** Two lines are printed from the
   pusher thread while the emulator is still running and writing the same UART,
   so those two lines can come out shot through with guest output. Everything
   after the halt has the UART to itself. Not a fault; do not chase it.
6. **Do not overwrite a known-good slot to test this** (gotcha 18). Push a small
   throwaway kernel first, confirm the halt, the reboot and the boot, and only
   then push anything you would mind losing.

---

## 2026-08-09: first hardware contact. Kernel works, 54 MB rootfs does not.

Two results, and the second one is the interesting one.

### Kernel push: works, end to end

`Image-virtionet`, 6,577,272 B into the kernel slot:

```
erasing 26 blocks (~21s modelled)   erase took 21s
payload sent in 34s (189.0 KB/s into the socket)
done in 1m52s (modelled 1m45s)      measured end-to-end 57.3 KB/s
```

Verified by the thing that matters: the board rebooted and the banner read
`kernel: 6577272 B, crc ok in 108 ms`, and that kernel is what is running now.
So the protocol, the halt, the erase, the write, the CRC and the container are
all correct on silicon, and `pushimage.py` drove it unmodified. **The
flash-rvlinux-push-flash-back dance is genuinely gone for kernel-sized images.**

That first kernel push needed widened timeouts (`--rx-timeout 120
--commit-timeout 300`). With defaults it failed at 99.6% — the tool finished
sending 3 MB ahead of the flash writer and timed out during the silent drain,
twice, identically (`rx error at payload offset 6561792 of 6577272`).

### The 16 KB receive window was the wrong fix

Reasoning at the time: rvlinux kept the sender blocked at the socket, so
`pushimage.py`'s timeout model assumes the wire paces itself; a big window
breaks that assumption. So cap it — `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=16384`
— and the sender's rate converges on the writer's.

**Measured: it made things worse.** With the cap, a 54 MB rootfs push stalled
at 0.06 MB (0.1%) instead of 99.6%, and attempts 2 and 3 never got a banner.
The hypothesis was at best incomplete and is not the explanation.

### The failure that matters: a 54 MB push takes the whole board down

The erase completed (217 blocks, 3m15s, modelled 2m57s). Payload started.
Then the board went **completely unreachable — no ping, no port 5555, silent
UART.** Not a stalled transfer: the application itself stopped.

It recovers cleanly on `probe-rs reset`, and the kernel slot survived intact,
so nothing is damaged. But this is a hard failure with a real fault behind it,
and it is NOT the historical intermittent verify miss.

What was ruled out, so nobody re-runs it:

| suspect | why it is not that |
|---|---|
| flash writer starving the network | writer is priority 8, below eth RX (2), TCP worker (2) and telnet (5) |
| the ECC-unit rule | the code never violated it; asserts and padding are guards, not fixes |
| the tool's timeouts | the board is unreachable, not slow — timeouts cannot cause that |
| image or slot geometry | identical image pushed fine through rvlinux minutes later |

Where to look next, in order: whether the RA OSPI-B driver spins with
interrupts masked for the duration of a program (12 minutes of near-continuous
programming is a very different duty cycle from a 26-block kernel, and it is
the one variable that scales with image size); whether the memory-mapped OSPI
window is disabled during programming while something still holds a pointer
into it (`plat_slot_base()` hands one to the machine layer, and the machine is
halted — but `img->rootfs` is still a live pointer in a struct); and the
watchdog, if one is enabled by default.

### Current honest scope

- **Kernel-sized images: use this pusher.** Proven on hardware.
- **54 MB rootfs: use the rvlinux path** (flash rvlinux, push, flash back).
  It did the same image in 14m21s the same day, before and after this attempt.

That is a smaller win than "gotcha 14 is dead", and it is the win that exists.

---

## 2026-08-09, later: code analysis of the 54 MB hang

No hardware, no builds. Read of the app, the RA OSPI-B driver and the generated
config. Three of the four suspects die here; the fourth is reframed.

### The premise in the section above is wrong, and it changes what to test

"The variable that scales is total programming time: ~12 minutes vs ~30
seconds" is contradicted by the measurement next to it. **The board died at
0.06 MB of 54 MB** — about seven 8 KB buffers, a few seconds into the payload.
It never reached twelve minutes of programming, or one minute.

And the payload write path is *proven good at that scale*: the kernel push
wrote 6.5 MB — ~800 buffers, ~100,000 page programs, ~80 s of continuous
programming — and only stopped because the host timed out at 99.6%.

So the thing that differs is not the payload phase. It is what immediately
precedes it. Per push:

| | kernel | rootfs | ratio |
|---|---:|---:|---:|
| erase blocks | 26 | 217 | 8.3x |
| erase wall time | 21 s | 3m15s | 9.3x |
| blank-check reads through the OSPI window | 6.8 MB | 56.9 MB | 8.3x |
| `sys_cache_data_invd_range` line ops | 212,992 | 1,777,664 | 8.3x |
| payload buffers actually written | ~800 | **~7** | — |

### It is a fault or a lockup, not a deadlock or starvation

Every wait on the push path is bounded, and each one prints:

- `k_sem_take(&nl_free, K_MSEC(15000))` → `pusher: STALLED, ...`
- `nl_recv()` → `zsock_poll(..., 30 s)` → `pusher: rx timeout` then
  `pusher: transfer died at payload offset N of M`

So a board that was merely stuck, starved or flow-controlled had to print
something within 30 seconds. It printed nothing, answered no ping, and came
back only on reset. That is the signature of an escalated HardFault (ARM
LOCKUP), and `CONFIG_HW_STACK_PROTECTION` is **not set** in this build, so a
stack overflow would also be silent rather than caught.

### Suspects (b), (c) and (d): dead, from the code

**(c) Watchdog — dead.** `# CONFIG_WATCHDOG is not set`,
`# CONFIG_TASK_WDT is not set`. There is nothing to starve.

**(b) Driver spinning with interrupts masked — not supported.**
`flash_renesas_ra_ospi_b_wait_operation()` is:

```c
while (status.write_in_progress) {
        R_OSPI_B_StatusGet(p_ctrl, &status);
        if (timeout == RESET_VALUE) { return -EIO; }
        k_sleep(K_USEC(50));
        timeout--;
}
```

It **sleeps**. At `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` each iteration yields
for at least one 100 us tick, so the writer thread hands the CPU to every other
thread on the board roughly ten thousand times a second for the whole push.
There is no `irq_lock()` and no busy-wait anywhere on the erase or write path.
Both operations are bounded and return `-EIO`, which this app reports on the
console — so a driver hang would have produced a line, not silence.

Worth recording while we are here: **the erase timeout margin is thin.**
`TIME_ERASE_256K` is 16000 iterations ≈ 1.6 s of budget, against a measured
899 ms per 256 KB block. Under 2x. Not the cause of this failure — the erase
completed — but it is the kind of margin that turns into an intermittent
`erase failed` on a hot or aging part.

**(d) Something in the double-buffer handoff after N iterations — dead.** There
is no per-iteration state but `i ^= 1` and two semaphores; nothing counts,
nothing wraps. And the evidence kills it independently: it died at ~7 handoffs
having survived ~800 the push before.

### Suspect (a): not supported from software, but not fully closable

`plat_slot_base()` has exactly six call sites — one in `main.c`'s boot check,
five in `pusher.c` (`img_check`, `img_diagnose`, the blank check, `img_commit`,
`nl_send_blkcrc`). **None of them can run while the writer is programming:**
the blank check finishes before `OK erased` goes out, and commit/blkcrc run
only after `k_thread_join()` has returned. The `img->rootfs` pointer the
machine layer holds is dereferenced only by the emulator, which is halted — and
the kernel push proves the halt works, because the board rebooted into the
image it had just been given.

So no *code path* in this application reads the window during programming.

What software cannot rule out: the OSPI window is Normal cacheable memory to
the MPU, and Armv8-M permits the core to make speculative reads there without
any instruction dereferencing a pointer. `R_OSPI_B_StatusGet()` uses a direct
transfer, which takes the controller out of memory-mapped mode, and the poll
loop above does that every ~100 us for the entire erase and the entire push. A
speculative read landing in one of those windows is a bus error the code cannot
prevent or see. **But that mechanism is equally live during a kernel push**,
which survives, so on its own it does not explain the split.

### Best-supported reading, and the confidence I have in it

A fault, taken at or just after the erase→payload transition, on a push whose
erase phase was 8x larger than the one that works. **I cannot name the faulting
access from the code, and I am not going to pretend otherwise — call it
moderate confidence in the *class* of failure and low confidence in any single
mechanism.** The useful output of this analysis is that it eliminates three
suspects and moves the search from the payload phase, where it was pointed, to
the erase and blank-check phase, where the 8x actually is.

### One decisive experiment, then a second

Both are Kconfig or argument changes only. No code change, no rebuild of the
push path logic.

**Test 1 — push a ~10 MB file to the *rootfs* slot.** Same slot, same start
address 0x841000, same write path; a 40-block/~35 s erase instead of
217-block/3m15s.

- Survives → the **erase length** (or the blank check that scales with it) is
  the variable. Slot address and write path are exonerated. Go to test 2.
- Dies → the **rootfs slot address** is the variable, not the size. That points
  at the region the guest had been reading in place, and makes the speculative
  variant of (a) the live hypothesis.

**Test 2 — `CONFIG_RVT_PUSHER_BLANK_CHECK=n`, push the 54 MB rootfs.**

- Survives → the blank check is implicated: 56.9 MB of memory-mapped reads plus
  a single `sys_cache_data_invd_range()` over 56,885,248 bytes, which is 1.78
  million cache-line operations in one unbroken loop. That is this app's code
  and this app's bug to fix.
- Dies → the erase itself is implicated, and it is a driver or silicon matter
  rather than an application one.

Test 1 first: it is the cheaper push and it splits the space better.

### Fixes worth making regardless of the outcome

1. **Instrument the first megabyte.** The progress line only prints every 1 MB,
   so the whole failure window — 0 to 0.06 MB — is a blind spot, and both
   attempts landed in it. Print at the start of the payload phase and every
   64 KB for the first MB. Without this the next run tells us as little as this
   one did.
2. **Make the blank check incremental.** Invalidate and check one 256 KB block
   at a time inside the erase loop rather than the whole span at the end. That
   turns one 1.78-million-operation cache call into 217 small ones, keeps the
   console alive through it, and would make a hang inside it land on a
   printable boundary. It is a better shape whether or not it is the bug.
3. **Revert `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=16384`.** Measured worse; the
   reasoning behind it was wrong.
4. Consider `CONFIG_HW_STACK_PROTECTION=y` for the next debug build. It costs an
   MPU region and turns a silent stack overflow into a printed fault, which is
   exactly the diagnostic this failure has been denying us.

### All four implemented, 2026-08-09

Built clean, both configurations. Details where they differ from the proposal:

**1. First-megabyte instrumentation — went finer than 64 KB, deliberately.**
The observed death was at **0.06 MB ≈ 62 KB, which is below a 64 KB mark**, so
64 KB granularity alone would still have printed nothing for either failure and
bought us another blind run. The payload phase now reports:

- one line when the payload phase begins, naming the flash address, the byte
  count and the buffer count — this alone separates "died in the erase
  aftermath" from "died in the payload phase", which the silent gap between
  `OK erased` and the 1 MB mark did not;
- **every buffer for the first 128 KB** (16 lines);
- every 64 KB to 1 MB, then every 1 MB as before;
- one line from the writer thread the first time `flash_write()` returns.

That last line is the one to look for. It splits "died before any payload write
completed" from "died after at least one did", and no evidence collected so far
answers that question.

**2. Blank check is now per 256 KB block, inside the erase loop.** One
`sys_cache_data_invd_range()` of 1.78 million line operations and 14.2 million
reads becomes 217 calls of 8192 and 65536, each between two printable points.
It also localises a bad block to the erase that produced it, so
`ERASE INCOMPLETE` now names the block address as well as the word.

**3. `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=16384` reverted**, with the reasoning
and the measurement that killed it left in `prj.conf` so nobody re-derives it.
The end-of-payload drain it was aimed at is a host-side timeout question,
answered with `--rx-timeout 120 --commit-timeout 300`.

**4. `CONFIG_HW_STACK_PROTECTION=y`, in the default build, not a debug-only
one.** The open bug is a silent lockup with no fault output, and a stack
overflow is one of the few faults that stays silent without this. It is also
cheaper here than the usual quote: on this Armv8-M core Zephyr satisfies it
with `CONFIG_BUILTIN_STACK_GUARD` — the PSPLIM hardware stack limit register —
**not** `CONFIG_MPU_STACK_GUARD`, so it costs a register write per context
switch and no MPU region. Checked in the generated `.config` rather than
assumed. Expect it to be able to turn an apparently-working board into one that
faults at boot; that would be a real overflow that was previously corrupting
memory quietly, not a regression.

### Documented risk: the erase timeout margin is under 2x

`TIME_ERASE_256K` in `flash_renesas_ra_ospi_b.h` is 16000, and
`flash_renesas_ra_ospi_b_wait_operation()` spends at least one 100 us tick per
iteration (`k_sleep(K_USEC(50))` at `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000`), so
the budget for one 256 KB erase is about **1.6 s**. Measured on this board:
**899 ms** per block (3m15s for 217 blocks).

That is a margin of 1.8x on a part whose erase time rises with temperature and
with program/erase cycles. It is not the cause of the 54 MB hang — the erase
completed — but when it does bite it will present as an intermittent
`pusher: erase failed at 0x...` partway through a long erase, on one block, and
it will look like a flash fault rather than a timeout. A 55 MB rootfs erase
rolls this dice 217 times per push.

Nothing to fix in this application: the constant is the driver's, and raising
it is a fork patch to `flash_renesas_ra_ospi_b.h` alongside the
`PAGE_SIZE_BYTE=64` one (mikeysklar/zephyr#11).

### Build after all four

Against the tree's current baseline, which now also carries the guest NIC
(189,948 B flash / 118,400 B RAM):

| | flash | RAM |
|---|---:|---:|
| before these four changes | 189,948 | 118,400 |
| **after** | **190,396** | **118,400** |
| delta | **+448** | **0** |

`arm-none-eabi-size`: 184,964 text / 5,504 data / 111,107 bss.
`-DCONFIG_NETWORKING=n` still builds: 93,064 / 2,060 / 23,010. No new warnings
in either.

### Test 1, ready to run

A ~10 MB push to the **rootfs** slot: same slot, same start address 0x841000,
same write path, but a 40-block/~35 s erase instead of 217 blocks/3m15s.

```sh
python3 ra8d1-linux/rvlinux/tools/pushimage.py <10MB-file> \
    --slot rootfs --tcp 192.168.2.3 \
    --rx-timeout 120 --commit-timeout 300 --retries 1
```

`--retries 1` on purpose: this is a measurement, not an attempt to land an
image, and a retry would muddy which attempt produced which console output.

Read the console, not the tool. The lines that decide it, in order:
`payload starts at flash 0x841000` → `8 KB` → `first flash_write returned` →
`16 KB` … Whichever of those is last is the answer.

- Survives → erase length (or the blank check that scaled with it) is the
  variable; slot address and write path are exonerated. Run test 2.
- Dies → the rootfs slot address is the variable, not the size.
- Dies **before** `first flash_write returned` → nothing to do with the write
  path at all, and the erase aftermath is the whole story.

---

## 2026-08-09: the instrumented 54 MB run. The board was never the problem.

The board did not wedge. It stayed alive, printed continuously, and ended with
its own diagnostics. Whether the earlier lockup was perturbed away or is simply
intermittent is **unknown and should stay unknown** until it recurs.

Everything below is read off
`scratchpad/push54-serial.log` and `scratchpad/push54.log`.

### There is no gap in the progress output

The reading that there were no 3/4/5/6 MB lines is a misread of the log. They
are all there, consecutive:

```
pusher: 1 MB at flash 0x00941000
pusher: 2 MB at flash 0x00a41000
pusher: 3 MB at flash 0x00b41000
pusher: 4 MB at flash 0x00c41000
pusher: 5 MB at flash 0x00d41000
pusher: 6 MB at flash 0x00e41000
pusher: rx error
pusher: transfer died at payload offset 6848512 of 56623104, flash 0x00ec9000
```

And the arithmetic closes exactly. Last print is payload offset 6,291,456
(0x00e41000 - 0x841000). Death is at 6,848,512 = 0x00ec9000 - 0x841000, which
is **557,056 bytes = 68 buffers later** — precisely what 1 MB granularity
predicts, since the next mark was not due until 7 MB. 6,848,512 is also
836 x 8192 exactly, so every buffer was full and no handoff was short.

The instrumentation did its job and there is nothing anomalous to explain in it.

### No wait in the pusher lacks a print, and the two prints that did NOT appear are the finding

Every blocking wait on the producer path is bounded and reports:

| wait | bound | prints |
|---|---|---|
| `k_sem_take(&nl_free, ...)` | 15 s | `pusher: STALLED, ...` |
| `nl_recv()` → `zsock_poll()` | 30 s | `pusher: rx timeout` |
| `zsock_recv()` | `SO_RCVTIMEO` 30 s | `pusher: rx error` |

**Neither `STALLED` nor `rx timeout` appeared.** So the flash writer was never
more than 15 seconds late with a buffer, and the board was never starved of
data for 30 seconds. What did appear is `rx error`, which is `nl_recv()`
returning -1: `zsock_poll()` reported ERR/HUP/NVAL, or `zsock_recv()` returned
zero or less. An orderly close and an RST both land there.

That is the host hanging up. The board was the victim of this failure, not the
cause of it.

### The tool log shows a healthy link running at exactly the flash write rate

The progress bar has two regimes, and reading them apart is the whole story:

```
 0.62/54.00 MB  elapsed 6s
 4.56/54.00 MB  elapsed 8s     <- +3.94 MB in 2 seconds
 ...
 6.50/54.00 MB  elapsed 36s
```

The jump is ~4 MB disappearing into socket buffers at wire speed. Everything
after it is honest: **4.56 MB to 6.50 MB in 28 s is 70.9 KB/s**, and the
measured flash write rate on this board is ~71 KB/s. From the moment the
buffers filled, the sender was paced precisely by the flash.

`pushimage.py` says so itself, in a comment: progress is "bytes accepted by the
socket, not bytes committed to flash." The kernel push showed the same gap and
the same rate — socket took 6.5 MB in 34 s, then 57 s of drain, which is
6.5 MB / 91 s = 71 KB/s.

### Whose clock is lying: neither

The board consumed **6,848,512 bytes, which is 32,768 MORE than the tool's last
printed 6.50 MB (6,815,744)**. The board therefore received everything the host
had handed to TCP *and* half of the 64 KB chunk the tool was blocked inside.
The host's send buffer was drained. There is no missing data and no
disagreement — the two numbers count different things and both are right.

### What actually failed: the host's own socket timeout, applied to send

`pushimage.py`'s `TcpBoard.__init__` does `self.s.settimeout(rx_timeout)`, and
in Python a socket timeout applies to `sendall()` as well as `recv()`.
CPython's `sock_sendall()` sets **one deadline for the entire call**. So
`--rx-timeout` is really "how long the host tolerates being flow-controlled",
and this link is *designed* to be slower than the sender.

**At 71 KB/s a 54 MB payload is 13.0 minutes of pure flash time**, on top of a
2m53s erase — a 16 minute operation. Any per-`sendall` timeout below that is a
gamble on how long the board's TCP window stays shut at the tail.

The one thing not provable from here: why a single 64 KB `sendall` failed to
complete in 120 s when the link had been moving 71 KB/s. The shape — the board
stopping at an exact buffer boundary having consumed everything available — is
what a zero-window stall looks like from the application's side, i.e. the
window closed and the reopen was not prompt. Stated as the shape of the
evidence, not as a diagnosis.

### Prediction for run 2 (`--rx-timeout 300`), written before the data

- Dies at an offset **substantially larger** than 6,848,512 (say past 12 MB) →
  the pause is finite and the host timeout was the entire story. Raise it and
  the push completes.
- Dies at a **similar** offset → the stall is not a pause but a deadlock, and
  more timeout will never fix it.
- Completes in ~16 minutes → same conclusion as the first case, with the image
  landed.

### What to do, cheapest first

1. **`--rx-timeout 600` for rootfs pushes.** A 54 MB push is a 16 minute
   operation; the tool has to be told to be patient. Zero code.
2. **Reconsider the receive window cap.** The evidence that condemned
   `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=16384` no longer decides anything: the
   "stalled at 0.06 MB" it was blamed for is what an *honestly paced* progress
   bar looks like in the first second, once the host is no longer allowed to
   buffer 4 MB ahead — 0.06 MB is roughly one second of flash at 71 KB/s. The
   other half of that verdict was the board wedge, which this run did not
   reproduce. A cap around 64 KB is worth one more measurement; it keeps the
   sender paced and stops the tail-end window games.
3. **The real fix is throughput, and it is already a known bug.**
   `PAGE_SIZE_BYTE=64` in `flash_renesas_ra_ospi_b.h` against a 256- or
   512-byte page buffer (mikeysklar/zephyr#11) is what makes this 71 KB/s
   instead of 4-8x that. Fixed, a 54 MB push goes from 13 minutes to about 3,
   and this entire class of timeout problem stops existing. It is a
   one-constant fork patch and it is worth more than any amount of tuning here.

### Also measured: the incremental blank check is 20x faster

`blank check ok, 965 ms over 217 blocks` — 4.4 ms per 256 KB block, 965 ms for
54 MB. The previous monolithic version accounted for roughly 19 s of the old
3m15s erase. Erase is now 2m53s for the same 217 blocks. The cost was never the
reading; it was the single `sys_cache_data_invd_range()` of 1.78 million
cache-line operations.

---

## 2026-08-09: run 2, and the actual difference from rvlinux

### First, my run-1 reading was incomplete, and run 2 says so

Run 1 looked like "healthy board, impatient host". The prediction written before
run 2 was: *dies at a similar offset → it is a deadlock, not a pause, and more
timeout will never help.* **That branch fired.** Run 2 had a 2.5x longer timeout
(300 s vs 120 s) and died **earlier** — offset 5,980,160 against 6,848,512 — with
the failure travelling in the opposite direction (`BrokenPipeError`, the board's
RST reaching the host, rather than the host's timeout reaching the board).

A longer timeout producing an earlier death rules out "the host was merely
impatient" as the whole story. The host timeout is real and still needs
`--rx-timeout 600`, but it is a symptom.

### One correction to the cluster before using it

The three deaths are quoted as 6,561,792 / 6,848,512 / 5,980,160. **The first one
does not belong.** 6,561,792 is 99.6% of a 6,577,272-byte image — it died
**15,480 bytes from the end of its own payload**, in the end-of-transfer drain,
which is a different event from dying at 11% of a 54 MB payload. Its offset is
near 6 MB because the *image* was 6.5 MB, not because 6 MB is a limit.

That leaves two real points, 5,980,160 and 6,848,512: **a 15% spread**. That is
the signature of a probabilistic failure — pressure that eventually loses a
dice roll — not of a resource that runs out at a fixed count.

### Run 2's timeline closes exactly

- Board consumed 5,980,160 B in 82 s = **71.2 KB/s**, the flash write rate.
- Tool's last progress: 9.69 MB at elapsed 1m22s = **82 s**. Same instant.
- Gap between them: 9.69 - 5.70 = **3.99 MB**, the host-side socket buffer,
  the same ~4 MB seen in run 1.

Both ends stopped together. Nothing stalled first.

### The RST is our own close, not a stack-initiated abort

`BrokenPipeError` on the host means the board sent RST. It did — but as a
consequence, not a cause. `nl_recv()` returned -1 (`pusher: rx error`),
`nl_push()` returned, and `zsock_close()` on a socket with unread data queued
emits RST. **The primary event is `zsock_poll()`/`zsock_recv()` failing the
socket**, i.e. Zephyr's TCP giving up on the connection.

### Net pool sizes are identical to rvlinux, so that is not it

Checked directly, `rvlinux/prj.conf` against `ra8d1-tinyemu/prj.conf`:

| | rvlinux | tinyemu |
|---|---|---|
| `NET_PKT_RX_COUNT` | 16 | 16 |
| `NET_PKT_TX_COUNT` | 16 | 16 |
| `NET_BUF_RX_COUNT` | 32 | 32 |
| `NET_BUF_TX_COUNT` | 32 | 32 |
| `NET_MAX_CONTEXTS` / `NET_MAX_CONN` | 6 / 6 | 6 / 6 |

Byte for byte the same. rvlinux pushes 54 MB through this silicon with these
numbers, so the pool sizes alone cannot be the explanation.

### What IS different: promiscuous mode clones every frame

`CONFIG_NET_PROMISCUOUS_MODE=y` is new in this app, for the guest NIC bridge.
rvlinux does not have it. And it is not a passive flag —
`subsys/net/ip/net_if.c:5946`:

```c
enum net_verdict net_if_recv_data(struct net_if *iface, struct net_pkt *pkt)
{
        if (IS_ENABLED(CONFIG_NET_PROMISCUOUS_MODE) &&
            net_if_is_promisc(iface)) {
                struct net_pkt *new_pkt;

                new_pkt = net_pkt_clone(pkt, K_NO_WAIT);
                ...
```

**Every frame the interface receives is deep-copied** — a second `net_pkt` and a
second full chain of `net_buf`s — before normal delivery. The arithmetic against
this build's pools is brutal:

| | |
|---|---:|
| `CONFIG_NET_BUF_DATA_SIZE` (fixed) | 128 B |
| net_bufs for one 1514 B frame | **12** |
| `CONFIG_NET_BUF_RX_COUNT` (whole RX pool) | **32** (4 KB) |
| one in-flight full frame **with** its promiscuous clone | **24 of 32** |
| frames per second at 71 KB/s | ~50 |

**One full-size frame in flight consumes three quarters of the RX buffer pool.
Two exhaust it.** That is the pressure a 54 MB push has to survive for a quarter
of an hour, and it is exactly the thing rvlinux does not do.

Failures under that pressure are dropped frames, retransmissions, and eventually
a connection Zephyr gives up on (`CONFIG_NET_TCP_RETRY_COUNT=9`) — which is a
probabilistic process with a 15% spread in when it bites, not a fixed limit. It
fits the two real data points and it fits rvlinux's success.

**Checked for a leak and there is not one.** When `net_pkt_clone()` fails under
pressure it returns NULL, `net_promisc_mode_input(NULL)` returns `NET_CONTINUE`
rather than `NET_DROP`, and the `net_pkt_unref()` is skipped — correctly, since
there is nothing to free. `nb_rx_thread()` unrefs on every path it takes. This
is allocation pressure, not accumulation; nobody needs to go hunting a leak.

### Fix: drop promiscuous mode when the guest stops

`rvt_netbridge_suspend()`, called from `halt_guest()` the moment the emulator
acknowledges the stop. With the guest halted the bridge has nobody to deliver
to, so every clone from that point on is allocated, filtered and freed having
achieved nothing. Turning it off removes the clone from the receive path for the
whole of the push and leaves the board's own IP traffic untouched.

There is no resume. The guest cannot run again without a reboot, so nothing
would consume what the bridge delivers.

**Deliberately the only change.** The obvious second lever — raising
`NET_BUF_RX_COUNT` from 32 to 128 and `NET_PKT_RX_COUNT` to 32, about 12 KB of
SRAM — is written down here and **not applied**, because changing both at once
means a successful next run tells us nothing about which one mattered. This
project has already spent a hardware cycle on a fix that was reasoned into place
without a controlled test. Promiscuous-off goes first because it is the only
receive-path difference from the configuration that demonstrably does 54 MB.

If the next 54 MB push still dies in the 5-7 MB band, the pool bump is the next
single change, and the one after that is `CONFIG_NET_BUF_DATA_SIZE` — at 128 B a
full frame costs 12 buffers, and 512 would cost 3.

### The wedge did not recur

Two full 54 MB attempts under `HW_STACK_PROTECTION=y` plus the payload
instrumentation, and the board stayed alive, printing and pingable through both,
ending each with its own diagnostics. The original symptom — no ping, no socket,
silent UART, reset-only recovery — **did not reproduce**. That is data, not a
verdict: nothing here explains what the wedge was, and no stack-overflow fault
was printed either, so "fixed" is not a claim anyone should make. If it returns,
the stack guard is now armed to name it.

### Build

| | flash | RAM |
|---|---:|---:|
| previous (four fixes) | 190,396 | 118,400 |
| **with promiscuous suspend** | **190,744** | **118,400** |

`arm-none-eabi-size`: 185,312 text / 5,504 data / 111,107 bss. Clean, no
warnings.

### The config comparison, done properly: generated .config, not prj.conf

`ra8d1-linux/rvlinux/build/zephyr/.config` against
`ra8d1-tinyemu/build-pusher/zephyr/.config`, every `CONFIG_NET_*` symbol.
**The complete networking delta is five symbols, and all five are the guest
NIC's:**

```
> CONFIG_NET_PROMISCUOUS_MODE=y
> CONFIG_NET_PROMISC_LOG_LEVEL=0
> CONFIG_NET_SOCKETS_PACKET=y
> CONFIG_NET_SOCKETS_PACKET_DGRAM=y
> CONFIG_NET_CONNECTION_SOCKETS=y
> CONFIG_NET_L2_ETHERNET_MGMT=y
> CONFIG_NET_ETHERNET_FORWARD_UNRECOGNISED_ETHERTYPE=y
```

**Not one TCP, pool, buffer or window symbol differs.** Every
`CONFIG_NET_TCP_*`, `NET_PKT_*`, `NET_BUF_*`, `NET_MAX_*` value is identical
between the app that moves 54 MB and the app that dies at 6.

#### Keepalive is definitively not it

It was a good shape to suspect — an unprovoked RST during sustained zero-window
is exactly what a misfiring keepalive looks like — but it is identical on both
sides, three ways:

| | rvlinux | tinyemu |
|---|---|---|
| `CONFIG_NET_TCP_KEEPALIVE` | y | y |
| `KEEPIDLE_DEFAULT` / `KEEPINTVL` / `KEEPCNT` | 7200 / 75 / 9 | 7200 / 75 / 9 |
| app sets `SO_KEEPALIVE` on the push socket | yes | yes |

`keep_alive_timer_init()` also starts each connection with
`conn->keep_alive = false` until `SO_KEEPALIVE` sets it, and the idle timer is
**7200 seconds** — two hours. It cannot fire inside a 16 minute push.

### What tcp.c actually does about zero windows

Read of `subsys/net/ip/tcp.c` in this tree:

- `tcp_send_zwp()` / `conn->persist_timer` is the **sender-side** zero-window
  probe: it runs when the *peer's* window is zero and we have data to send. On
  a push the board is the receiver, so this path belongs to macOS, not to us.
- `conn->zwp_retries` saturates at 63 and only stretches the probe backoff. It
  never closes a connection. There is no receive-side probe counter that gives
  up.
- The only places tcp.c aborts a connection of its own accord are keepalive
  exhaustion (`keep_cur > keep_cnt` → `-ETIMEDOUT`, ruled out above), the
  SO_LINGER timeout (`CONFIG_NET_CONTEXT_LINGER` **is not set** in this build),
  and retransmission exhaustion (`CONFIG_NET_TCP_RETRY_COUNT=9`).

**So the reachable mechanism is retransmission exhaustion**, which is what
sustained frame loss produces — and frame loss is what an RX pool under the
promiscuous clone's pressure produces. No counter in tcp.c fires at a fixed
byte count, which agrees with the 15% spread between the two observed deaths:
this is loss and retry, not a limit being hit.

Note also that the board's RST is our own `zsock_close()` on a socket with
unread data queued, after `nl_recv()` already returned -1. Zephyr failed the
socket first; the RST is the tail of that, not an unprovoked abort.

### The discriminating change, and what it should do

**Suspend promiscuous mode when the guest halts** — implemented above. It is
the cheapest discriminator available because it removes the *only* per-packet
receive-path difference from the configuration that demonstrably moves 54 MB
through this silicon. After the halt the board's RX path is byte-for-byte
rvlinux's.

Expected result if the diagnosis is right:

| outcome | reading |
|---|---|
| push completes (~16 min) | confirmed; ship it |
| dies **well past** the 5-7 MB band (>20 MB) | cloning was most of it, something else remains |
| dies **again at 5-7 MB** | cloning is exonerated, and the promiscuous theory is dead rather than dented |

That third row is the point of doing this one change alone. Ranked next levers,
one at a time, in this order:

1. `CONFIG_NET_BUF_RX_COUNT` 32 → 128 (~12 KB SRAM). Raises a 4 KB RX pool that
   holds 2.6 full-size frames.
2. `CONFIG_NET_BUF_DATA_SIZE` 128 → 512. A 1514 B frame costs 12 buffers at 128
   and 3 at 512; fragmentation is half the reason the pool is tight.
3. `CONFIG_NET_ETHERNET_FORWARD_UNRECOGNISED_ETHERTYPE=n` if it can be turned
   off without breaking the bridge — it makes L2 pass frames up that it would
   otherwise drop, which is more net_pkt churn on a shared segment.

A receive-window cap is **not** on that list any more. It was tried, it
measured worse, and the reason it looked worse is now understood: it removed
the host-side buffering that was masking how slow the link really is.

### Three things to keep from this episode

**The real throughput fix is `PAGE_SIZE_BYTE`.** `flash_renesas_ra_ospi_b.h`
hardcodes 64 against an S28HL512T page buffer of 256 or 512
(mikeysklar/zephyr#11). That one constant is why a push runs at 71 KB/s and why
54 MB is a 13-minute exposure to any network fault at all. Fixed, the same push
is about 3 minutes, and every timeout, window and pool problem in this file gets
4-8x less time to happen. It is worth more than anything else listed here.

**The incremental blank check was worth doing on its own merits.** 965 ms over
217 blocks (4.4 ms per 256 KB) against roughly 19 s for the single-pass version.
The cost was never reading the flash; it was one
`sys_cache_data_invd_range()` of 1.78 million cache-line operations. Erase went
from 3m15s to 2m53s for identical work.

**The wedge has not recurred.** Two full 54 MB attempts under
`HW_STACK_PROTECTION=y` with the payload instrumentation, and the board stayed
alive, printing and pingable through both, ending each with its own
diagnostics. No stack-overflow fault printed either. That is two clean runs, not
an explanation — nothing here accounts for the original no-ping/silent-UART
lockup, and it should not be called fixed. The guard is armed to name it if it
returns.

---

## 2026-08-09, run 5: the cloning hypothesis is refuted, and the real mechanism

### Scoring my own hypothesis: wrong

I predicted three outcomes for a promiscuous-suspended push: completes, dies
well past 6 MB, or **"dies again at 5-7 MB → cloning is exonerated and the
theory is dead rather than dented."** Run 5 died at **1,048,576** — not past the
band, *below* it, six times lower, with cloning off and no guest running.

**That refutes cloning as the mechanism.** Not weakened, refuted. The clone is
still real waste and suspending it is still correct, but it is not what kills
these pushes and it must not be described as a fix.

The death-offset table is the argument:

| offset | buffers | promiscuous | guest |
|---:|---:|---|---|
| 6,848,512 | 836 | on | running |
| 5,980,160 | 730 | on | running |
| 1,048,576 | 128 | **off** | **none** |

A 6.5x spread with the suspected cause removed. This was never a counter or a
pool running out at a fixed point.

### (a) No, there is no 7-bit or 1 MB state in the pusher

Rechecked against the instrumented version specifically. The producer's entire
per-iteration state is `i ^= 1`, `have`, `want`, and `off`; the erase loop's is
`dots`. All `uint32_t`, none masked to 7 bits. The print condition is

```c
off <= (128U << 10) ||
(off < (1U << 20) && (off & ((64U << 10) - 1)) < CONFIG_RVT_PUSHER_BUF) ||
(off & ((1U << 20) - 1)) < CONFIG_RVT_PUSHER_BUF
```

which at `off == 1048576` takes the third arm, prints `1 MB`, and touches
nothing else. The `1 MB` line **did** print, so iteration 128 completed in full
— recv, handoff, `off += 8192`, print — and the failure is in iteration 129's
receive. The print granularity changes at that offset because the transfer
stopped there, not the other way round.

128 buffers being 2^7 is coincidence. The other two deaths were at 836 and 730
buffers, which are not powers of anything. All three are multiples of 8192
because `off` only ever advances by a completed buffer.

### (b) What the reader was blocked in — and it is a defect of mine

Enumerating every wait, the answer is none of them. It was **not** blocked. It
was looping.

```c
while (have < want) {
        int got = nl_recv(s, buf + have, want - have, 30 /* seconds */);
        ...
        have += got;
}
```

`nl_recv()` gives **each call** a fresh 30 s poll. The loop that fills a buffer
had **no overall deadline**. So a sender delivering any bytes at all more often
than every 30 seconds keeps this loop alive indefinitely:

- `off` never advances → no progress line, at any granularity
- the 15 s `nl_free` stall never fires → the writer has nothing pending
- the 30 s poll never expires → it keeps being satisfied

**That is the universal signature of all four failures, exactly.** Board
consumed everything sent, host blocked in `sendall` for 600 s, board silent,
neither timeout printed. The board was awake and spinning on a trickle the whole
time.

### (c) The trickle is a TCP zero-window persist probe, and the window is the bug

macOS uses BSD persist timing: `TCPTV_PERSMIN` 5 s to `TCPTV_PERSMAX` 60 s with
exponential backoff, each probe carrying **one byte**. Delivered to the app,
that is one byte per `nl_recv()` — enough to reset a 30 s poll and never enough
to fill an 8192-byte buffer. At one byte per probe a buffer takes eleven hours.

Why the connection is in zero-window state essentially permanently:

```c
static int tcp_rx_window =
#if (CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE != 0)
        CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE;
#else
        (CONFIG_NET_BUF_RX_COUNT * CONFIG_NET_BUF_DATA_SIZE) / 3;
```

With this build's `NET_BUF_RX_COUNT=32` and `NET_BUF_DATA_SIZE=128`:

**(32 x 128) / 3 = 1365 bytes. The board's entire TCP receive window is 1365
bytes — 0.93 of one MSS.**

The window cannot hold a single full-size segment. Every segment the host sends
closes it completely, and every subsequent segment depends on a window-update
ACK going out and arriving. A 54 MB push is roughly 40,000 of those round trips,
and losing or suppressing **one** leaves the sender in persist mode forever:
the app has already drained everything, so `tcp_update_recv_wnd()` — which is
only called from `net_context.c:4263` when the application reads *more* data —
is never called again, and no further window update is ever generated. The
deadlock is self-sustaining.

That is a per-round-trip lottery, which is why the death offsets scatter over
6.5x instead of clustering on a count.

**And it reframes the 16 KB experiment completely.** Setting
`CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=16384` did not *cap* anything — a nonzero
value **replaces** the derived one, so it raised the window from 1365 to 16384,
**four times the entire 4 KB RX buffer pool**. The board advertised four times
the buffering it owned. That is why it measured worse, and the "cap" reading of
that result was wrong in both direction and magnitude.

### The fix to make next, and why it is two symbols not one

The window is derived from the buffer pool, so the pool and the window have to
move together:

```
CONFIG_NET_BUF_RX_COUNT=256      # 32 KB of RX buffer (was 4 KB)
CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE=10922   # or leave 0 and let it derive
```

At `NET_BUF_RX_COUNT=256` the derived window is `(256*128)/3 = 10922` bytes =
7.5 MSS, backed by a pool three times its size. Costs about 28 KB of SRAM, which
this app has: RAM is at 12.9% of 896 KB.

Intermediate points, if 28 KB is judged too much: 64 → 2730 B (1.9 MSS), 128 →
5461 B (3.7 MSS). **Anything at or above 2 MSS gets the connection off the
knife edge**, because the window can then hold a whole segment plus room to
acknowledge it without going to zero every time.

rvlinux runs the same 1365-byte window and pushes 54 MB successfully, so this
is a probability, not a certainty — it lives on the same knife edge and gets
lucky. That is consistent with everything seen: rvlinux's pushes are also
described in this repo as intermittently failing.

### Instrumented for the next run

`CONFIG_RVT_PUSHER_BUFFER_MS` (default 60 s) bounds filling one buffer, not just
one recv. On expiry it prints bytes received, elapsed, recv-call count and bytes
per call:

```
pusher: BUFFER STALLED -- 37 of 8192 bytes in 60013 ms over 37 recv calls
(1 bytes each). One byte each is a zero-window persist probe: the sender is
blocked and our window never reopened
```

**One byte per call proves the persist-probe diagnosis outright.** Anything
else — a few hundred bytes per call, or zero calls — refutes it and says look
elsewhere. A healthy buffer arrives in ~115 ms, so 60 s is 500x headroom.

Build: 191,376 B flash, 118,400 B RAM, clean.

### For whoever picks this up tomorrow

Start at the window-update question, not at the beginning.

1. The board's receive window is **1365 bytes, below one MSS**, derived from
   `NET_BUF_RX_COUNT * NET_BUF_DATA_SIZE / 3`. Everything else follows from
   that.
2. The next run's `BUFFER STALLED` line settles whether persist probes are the
   trickle. Read that line first.
3. If it is: the question is why Zephyr stops emitting window updates.
   `tcp_update_recv_wnd()` is called on the receive path with a negative delta
   (tcp.c:1342) and from the application read path (net_context.c:4263) with a
   positive one; the emission condition is
   `(short_win_before && !short_win_after) || tcp_need_window_update(conn)` at
   tcp.c:1250. With `recv_win_max` 1365 and `conn_mss` 1460, look hard at
   `tcp_need_window_update()`'s `MAX(conn_mss, recv_win_max/2)` threshold — it
   is 1460, larger than the whole window, which makes that predicate behave in
   ways its author did not have in mind.
4. Raise the pool before spending another hardware window on anything subtle.
   It is two lines and it moves the system off the edge the bug lives on.
5. `PAGE_SIZE_BYTE` (#11) still shortens every exposure by 4-8x and remains the
   highest-value single fix in this whole file.

---

## 2026-08-09, runs G and H: the first complete payload, and the historical fault caught in the open

### Run G: window fix live, a new failure mode

`NET_BUF_RX_COUNT=256`, window 10922 B, verified in autoconf. Erase 3m23s,
blank check clean. Died at **475,136** — 58 buffers, 464 KB — with a bare
`rx error`, and **`BUFFER STALLED` never fired**.

That silence is the instrument working, not failing. `BUFFER STALLED` only
fires when a buffer takes over 60 s to fill; a hard `rx error` inside 60 s of
healthy flow means `zsock_poll()` or `zsock_recv()` failed at the stack level.
**This is not the trickle deadlock.** It is a different mode, at an offset well
below the old 5-7 MB band, and its identity is unknown because the bare print
predates the errno patch. The errno instrument is armed for the next one.

One data point. Do not build a theory on it yet.

### Run H: 54 MB of payload, clean, first time ever

```
payload sent in 6m41s (137.8 KB/s into the socket)
pusher: 56623104 bytes in 402291 ms
```

**The entire payload streamed and was written without a transport failure.** No
stall, no trickle, no early death. That has not happened before in this app.

#### Scoring the window fix honestly: n=1 clean of 2, and do not credit it with the speed

What the window fix can be credited with: the trickle-deadlock signature —
silent board, `off` frozen, host parked in `sendall` — **did not recur in either
run**, and `BUFFER STALLED`, built specifically to catch it, never fired.

What it must **not** be credited with: the 1.94x throughput. 71 KB/s → 137.8
KB/s looks like a window effect and is almost certainly the WIP polling floor
(fork `122c70fe591`), which landed in the same builds. **Two variables moved
together, so the throughput number cannot be attributed to either.** If the
attribution matters, one build with the floor and the old window settles it;
nothing downstream depends on knowing.

Run G is the honest asterisk: one clean payload out of two attempts on this
build. Possibly residual probabilistic failure, possibly a distinct bug.

### The verify failure is the historical fault, and three instruments cornered it

```
block diff: 1 of 864 64 KB blocks differ -- block 125 at payload 0x7d0000
DIAG: 0 of 864 blocks read differently on a second pass
DIAG: 0 of 864 64K blocks still erased (0xFF)
DIAG: readback crc pass1=0x0d0e93ba pass2=0x0d0e93ba stable
pusher: blank check ok, 952 ms over 217 blocks
```

BUILD.md has carried this fault since before this port: "the board writes the
full payload, reads it back, and the CRC sometimes disagrees", with two live
hypotheses — an OSPI read/write timing margin, or an incomplete erase. **Both
of those are now eliminated, and for the first time the evidence does it
positively rather than by argument:**

| instrument | result | eliminates |
|---|---|---|
| blank check, per 256 KB block after erase | all 217 blocks verified 0xFF | **incomplete erase** |
| DIAG two-pass CRC with cache invalidate | 0 of 864 blocks differ, pass1 == pass2 | **stale read / OSPI prefetch** |
| DIAG erased-block scan | 0 of 864 still 0xFF | **a block that was never programmed** |
| host block diff | 863 of 864 byte-correct | **transport corruption** |

By elimination the program operation for that one block wrote the wrong data
and the flash has held it faithfully ever since. **Written, stable, wrong** —
and now with the erase and the read path ruled out underneath it.

That is what the blank check and the two-pass DIAG were added for, and it is
the first time they have had the chance to do it.

#### One precision point: "scattered" is over-reading a single block

`pushimage.py` printed `shape: scattered -- individual flash operations failing
independently`. That verdict comes from `len(runs) > len(bad) // 2`, which for
one bad block is `1 > 0` — **trivially true for any single failure.** With n=1
you cannot distinguish scattered from isolated, and the tool's own third
category is the right reading: "a single bad block in an otherwise perfect image
is something else again."

Geometry, which also argues against an erase-side effect:

- block 125 spans payload 0x7d0000-0x7e0000 = **buffers 1000-1007** of 6912
- flash address 0x1011000
- that sits **25.6% into erase block 31** of 217 — nowhere near an erase-block
  boundary, so this is not an edge-of-erase artefact

**The discriminator is free and already running.** Run I retries the same image:

- fails again at **block 125 / flash 0x1011000** → a physical defect or marginal
  cell at that address, and the fix is a bad-block skip or simply a different
  slot offset
- fails at a **different** block → a transient program fault, and the address
  carries no information
- **passes** → intermittent at roughly the historical rate, and `--retries 3`
  remains the answer

No new code is needed to learn that, which is why it beats any instrument that
could be added now.

### Open questions, ranked

**1. The residual single-block program fault.** The WIP floor is in place, so
"the next program started before the last one finished" is less likely — but a
floor is a floor, not a proof. `flash_renesas_ra_ospi_b_wait_operation()`
returns as soon as `R_OSPI_B_StatusGet()` reports `write_in_progress` clear, and
that status read is itself a `DirectTransfer` that takes the controller out of
memory-mapped mode. A status read that returns a stale or mis-sampled bit lets
the next program start early, with the WIP floor unable to see it. Suspects, in
order:

  - status-read / mode-switch interaction letting a program start early
  - a marginal cell, i.e. Semper ECC correcting-then-failing at one address —
    **repeatable at the same address, which run I tests for free**
  - the FSP combination-write path racing something at 64-byte page granularity

A 64 KB block is 1024 page programs at `PAGE_SIZE_BYTE=64`. One of 1024 went
wrong; localising further than 64 KB needs finer CRCs than the protocol carries,
and changing the protocol means changing `pushimage.py`, which is the one
component known to work. **Prefer the retry experiment over new instrumentation
here.**

**2. Run G's hard `rx error` at 464 KB.** Unidentified. The errno is now in the
report, so the next occurrence names itself. Until then it is one data point and
should not be merged with the trickle story — `BUFFER STALLED` staying silent is
positive evidence that it is a different mechanism.

### Practical state

**`--retries 3` is the operating procedure**, and it always was the design
assumption — the retry loop exists because this fault is intermittent rather
than deterministic. A retry re-erases the slot before re-sending, which is also
exactly what the Semper ECC rule requires: nothing is ever re-programmed in
place.

A 54 MB push is now ~3m30s of erase plus ~6m40s of payload, about 10-11 minutes
per attempt. Budget three attempts.

`PAGE_SIZE_BYTE` (#11) remains the highest-value single fix in this file: at 64
against a 256- or 512-byte page buffer it is still costing 4-8x on every write,
and every minute it adds is a minute of exposure to everything above.
