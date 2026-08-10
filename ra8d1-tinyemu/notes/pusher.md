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
