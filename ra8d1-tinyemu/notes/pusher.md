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
