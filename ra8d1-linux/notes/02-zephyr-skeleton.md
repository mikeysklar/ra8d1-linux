# 02 - Zephyr app skeleton on EK-RA8D1

Standalone Zephyr application in `ra8d1-linux/zephyr-app/`. Nothing under
`circuitpython/ports/zephyr-cp/` was modified; that tree is only used as the west
workspace and Zephyr source.

Status: **builds, flashes, runs. All 64 MiB of SDRAM verified with 0 errors. CPU
confirmed at 480 MHz.**

## Files

| file | purpose |
| --- | --- |
| `zephyr-app/CMakeLists.txt` | stock Zephyr app boilerplate |
| `zephyr-app/prj.conf` | `CONFIG_MEMC=y` plus I/D-cache on |
| `zephyr-app/nocache.conf` | overlay that turns the D-cache off, for the comparison run |
| `zephyr-app/src/main.c` | banner, clock report, timer report, five SDRAM tests |
| `zephyr-app/flash.sh` | objcopy-strip `.option_setting*` then probe-rs download + reset |
| `zephyr-app/console.py` | raw 115200 dump of the probe VCOM |

## Build

```sh
source /Users/sklarm/Downloads/ada/siwx917/env.sh
source /Users/sklarm/Downloads/ada/siwx917/.venv/bin/activate
cd /Users/sklarm/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp

west build -p always -b ek_ra8d1 \
  /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app \
  -d /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app/build

# D-cache-off comparison build
west build -p always -b ek_ra8d1 \
  /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app \
  -d /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app/build-nocache \
  -- -DEXTRA_CONF_FILE=nocache.conf
```

Clean build first try, no warnings from our code. Footprint: 38.7 KB flash,
8.7 KB RAM out of 2016 KB / 896 KB.

## Flash and console

```sh
/Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app/flash.sh            # default build
/Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app/flash.sh <other.elf>
/Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/zephyr-app/console.py 30
```

`flash.sh` strips every `.option_setting*` section into `/tmp/ra8d1-app-noofs.elf`
before `probe-rs download`, because probe-rs cannot map that flash region. It uses
probe `1366:0105:001086859839` and chip `R7FA8D1BH`. Do not use west/JLinkExe:
JLinkExe force-updates the on-board probe firmware.

Console is `/dev/cu.usbmodem0010868598391` at 115200.
`/dev/cu.usbmodem0004403537871` is the SiWx917 board, leave it alone.

## SDRAM enablement

The board DTS already has everything:

- `zephyr/boards/renesas/ek_ra8d1/ek_ra8d1.dts:92` - `sdram1: sdram@68000000`,
  `zephyr,memory-region = "SDRAM"`, `reg = <0x68000000 DT_SIZE_M(64)>`
- same file line 426 - `&sdram` with pinctrl, timings, `bus-width = "16-bit"`,
  `status = "okay"`

The only thing the app has to add is `CONFIG_MEMC=y`. `MEMC_RENESAS_RA_SDRAM`
then defaults to `y` off `DT_HAS_RENESAS_RA_SDRAM_ENABLED` and pulls in
`USE_RA_FSP_SDRAM`. Verified in the generated `.config`.

No MPU work was needed. 0x68000000 falls in the ARMv8-M default background map's
normal-cacheable RAM window and `CONFIG_MPU_DISABLE_BACKGROUND_MAP` is off.

## Clocks (read back from live SCKDIVCR/SCKDIVCR2 via FSP, not from Kconfig)

```
CPUCLK (core)                       480000000 Hz    <- 480 MHz CONFIRMED
ICLK   (system bus)                 240000000 Hz
BCLK   (external bus / SDRAM)       120000000 Hz
SystemCoreClock (CMSIS)             480000000 Hz
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC  480000000 Hz
```

Gotcha: `R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK)` returns 240 MHz. That is
correct and is *not* the core clock. On RA8 the core runs off CPUCLK, which is a
separate divider in SCKDIVCR2. Ask for `FSP_PRIV_CLOCK_CPUCLK`.

Timer cross-check: `k_busy_wait(500000)` elapsed 500 ms of `k_uptime_get()` and
240,000,360 kernel cycles, implying 480.0007 MHz. Kernel tick rate is 10 kHz.
`k_uptime_get()` and `k_cycle_get_32()` both work; note the 32-bit cycle counter
wraps every ~8.9 s at 480 MHz, so the long tests are timed off `k_uptime_get()`.

**BCLK = 120 MHz on a 16-bit bus is a hard ceiling of 240 MB/s for SDRAM.**
Everything below should be read against that number.

## SDRAM results

Region: `/sdram@68000000`, base 0x68000000, 67108864 bytes.

Tests run, all over the **full 64 MiB** unless noted:

1. byte and halfword access (16-bit bus, so partial writes exercise the byte-enable path)
2. unaligned 32-bit access at every byte offset, plus cache-line straddling
3. write bijective pattern `(i * 0x9E3779B1) ^ (i >> 7) ^ salt` over all 16.7 M words,
   cache flush+invalidate, then verify all 16.7 M words
4. same again with the inverted salt
5. sparse signature over 4096 probes, 2 s sleep, re-verify (proves auto-refresh)
6. bandwidth: sequential store, sequential load, memset, memcpy, strided load

### Correctness

```
byte/halfword access: ok (0 failures)
unaligned access:     ok (0 failures)
pass 1 (salt 0x00000000): 0 mismatches over 16777216 words
pass 2 (salt 0xFFFFFFFF): 0 mismatches over 16777216 words
retention after 2 s:  ok (0/4096 probes bad)
RESULT: all 64 MiB of SDRAM verified, 0 errors
```

### Unaligned access and MPU typing

The emulator issues unaligned 32-bit accesses. On Cortex-M85 those are legal
only against Normal memory; against Device memory they are an UNALIGNED
UsageFault. Checked directly on hardware:

```
MPU_CTRL   0x00000005 (ENABLE=1 PRIVDEFENA=1), 8 regions
SCB_CCR    0x000f0211 (UNALIGN_TRP=0)
no MPU region covers 0x68000000; falls through to the default background map,
where 0x60000000-0x7fffffff is Normal WBWA.

offset +1: ok
offset +2: ok
offset +3: ok
cache-line-straddling (+30 of every 32): ok
unaligned u32 load: 4194304 reads in 125 ms -> 128.00 MiB/s
```

So unaligned works, at full aligned-read speed (128 vs 123 MiB/s), no fault.
Verified in the disassembly that the packed-struct accessor really emits a
single unaligned `ldr.w r1, [r2], #4` from an odd base (`0x68000001`,
`0x68002001`), not a byte-assembly sequence, so the hardware path is genuinely
exercised.

**This holds only because nothing maps the region.** `PRIVDEFENA=1` and all 8
MPU regions miss 0x68000000, so the ARMv8-M default background map applies and
types it Normal WBWA. If anyone later adds an MPU region over the guest RAM
window (see the cacheability idea below), it **must** be typed Normal. Typing it
Device to defeat the cache would make every unaligned guest access hard-fault.

The pattern is bijective in the word index, so a stuck or shorted address line
would surface as a data mismatch rather than aliasing silently. Both salts pass,
which also rules out stuck data bits. The cache is flushed and invalidated
between the write and verify passes, so the verify genuinely reads the SDRAM
parts and not dirty cache lines.

### Throughput

| operation | D-cache on | D-cache off |
| --- | --- | --- |
| sequential u32 store, 64 MiB | **150.6 MiB/s** (425 ms) | 150.6 MiB/s (425 ms) |
| sequential u32 load, 64 MiB | **123.1 MiB/s** (520 ms) | 32.5 MiB/s (1971 ms) |
| volatile u32 write, 64 MiB | 150.6 MiB/s (425 ms) | 150.6 MiB/s (425 ms) |
| volatile u32 read+compare, 64 MiB | 124.8 MiB/s (513 ms) | 25.4 MiB/s (2520 ms) |
| memset, 64 MiB | 150.2 MiB/s (426 ms) | 150.2 MiB/s (426 ms) |
| memcpy, 32 MiB moved | 31.3 MiB/s (1023 ms) | 20.6 MiB/s (1551 ms) |
| 1 word per 32 B line, 2.10 M lines | 4.21 M acc/s (498 ms) | **8.53 M acc/s** (246 ms) |
| unaligned u32 load, 4.19 M reads | 128.0 MiB/s (125 ms) | not measured |

Repeatable to within a few ms across runs and across both write passes.

Read those against the 240 MB/s theoretical ceiling: streaming writes hit 158 MB/s
(66% of peak) and streaming reads 129 MB/s (54%). For an SDRAM controller on a
16-bit bus that is a reasonable fraction, not obviously leaving much on the table.

### What matters for the emulator

- **Reads are the thing the D-cache buys.** 123 vs 32 MiB/s, a 3.8x swing.
  Keep `CONFIG_DCACHE=y`.
- **Writes do not care about the D-cache at all.** 150.6 MiB/s either way, to
  three digits. The store path saturates the same SDRAM write bandwidth whether
  or not lines are allocated. Convenient, and it means write-heavy guest code is
  not going to be helped by cache tuning.
- **Scattered access inverts the answer.** One word per 32-byte line runs
  *twice as fast with the D-cache off* (8.53 vs 4.21 M accesses/s), because a
  cached miss drags in 32 bytes to use 4. If the emulator ends up chasing guest
  page tables or doing scattered guest loads, the cache is a 2x tax on exactly
  those accesses. Worth revisiting once there is a real workload: the right
  answer may be D-cache on with the guest RAM window marked non-cacheable via
  an MPU region, or vice versa. If that MPU region gets added, type it Normal
  Non-cacheable, **not** Device, or unaligned guest accesses start faulting.
- **Unaligned guest accesses are free.** 128 MiB/s, indistinguishable from
  aligned. No need to make the emulator split them.
- **memcpy is the weak spot.** 31 MiB/s cached is far below what store (150) and
  load (123) predict for a copy (~68 MiB/s if perfectly serialised). Some of the
  gap is write-allocate pulling destination lines in before overwriting them,
  which would add a third 32 MiB of traffic. That does not fully explain it and
  picolibc's memcpy is a suspect too. Flagging rather than explaining: if the
  emulator does bulk guest-memory moves, hand-writing that loop is likely worth
  a couple of x.
- Budget arithmetic: at ~123 MiB/s of guest-memory reads and 480 MHz core, a
  RISC-V interpreter doing one guest load per ~50 host cycles is nowhere near
  memory-bound yet. Memory bandwidth becomes the wall only once the interpreter
  gets under ~30 host cycles per guest memory op.

## Failures

None. Everything asked for works. Two things that cost time and are worth
recording:

1. `FSP_PRIV_CLOCK_ICLK` reports 240 MHz and reads like a failed 480 MHz check.
   It is the bus clock. Use `FSP_PRIV_CLOCK_CPUCLK`.
2. First version accumulated the read-loop checksum with XOR. After the memset
   pass every word is 0x5A5A5A5A, so the checksum XORed itself to 0 and looked
   like the compiler had deleted the loop. Switched to `acc = (acc << 1) + v`.

## Next

The skeleton does what it needs to. Open questions for the emulator work:
MPU-region cacheability policy for the guest RAM window, a hand-written bulk
copy, and whether the 64 KB ITCM / 64 KB DTCM (both currently unused, see the
build's memory report) should hold the interpreter loop and the hot guest state.
