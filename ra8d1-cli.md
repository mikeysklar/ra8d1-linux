# EK-RA8D1 CircuitPython bring-up — command runbook

Board: Renesas EK-RA8D1. SoC `R7FA8D1BH` (Cortex-M85 @ 480 MHz). Zephyr board
`ek_ra8d1`, CircuitPython board `renesas_ek_ra8d1`. Started 2026-08-07.

Branch: **`ra8d1/integration`** on `mikeysklar/circuitpython`, based on adafruit
main `869d51b20d` with four patches from Hermes's pack applied. Deliberately
separate from the SiWx917 work. Zephyr pinned to `adafruit/zephyr@63f054c88ee`
via the branch's own `west.yml`.

Working state as of 2026-08-07: **boots, live REPL, 63 MB heap, MIPI panel
rendering.** See "Measured on hardware" and "Open items" below.

## 0. Env

```sh
source ~/Downloads/ada/siwx917/env.sh
source ~/Downloads/ada/siwx917/.venv/bin/activate
```

Switching between this and SiWx917 work moves the shared zephyr checkout.
`west update` detaches it to the manifest rev; branches are preserved:

```sh
git -C zephyr checkout siwx917/fork-ci-integration   # back to SiWx917 work
```

## 1. Identify the probe

Two J-Link boards on this bench — match on **vendor**, not just "a J-Link".

```sh
ioreg -p IOUSB -l -w 0 | grep -E '"USB Product Name"|"USB Vendor Name"|"USB Serial Number"'
ls /dev/cu.* | grep -vi "bluetooth\|debug-console"
```

| Board | Vendor | Serial | VCOM |
|---|---|---|---|
| SiWx917 DK2605A | Silicon Labs | `000440353787` | `cu.usbmodem0004403537871` |
| EK-RA8D1 | **SEGGER** | `001086859839` | `cu.usbmodem0010868598391` |

(`system_profiler SPUSBDataType` returns empty on this Mac — use `ioreg`.)

## 2. Verify toolchain handles Cortex-M85

```sh
arm-none-eabi-gcc -mcpu=cortex-m85 -E - </dev/null >/dev/null && echo SUPPORTED
```

## 2b. Branch setup (one time, already done)

The four patches from the pack apply cleanly to adafruit main. `869d51b20d` is
both the pack's stated base and the current tip of upstream main.

```sh
cd ~/Downloads/ada/siwx917/circuitpython
git fetch origin main
git checkout -b ra8d1/integration 869d51b20d
git submodule update --init lib/mbedtls        # branch switch leaves it stale
cd ports/zephyr-cp && west update zephyr       # this branch pins a different zephyr
cd ../.. && git am ~/Downloads/ada/siwx917/ra8d1-pack/patches/*.patch
```

What the patches do, and which actually matter for first boot:

| | | |
|---|---|---|
| 0001 | oofatfs valid FAT boot jump | correctness only, **not** a mount fix (see below) |
| 0002 | `CONFIG_RENESAS_RA_GLCDC_FB_NUM=1` | **required** — without it the board hangs on first display refresh |
| 0003 | mirror console to the chosen UART | **required** — without it there is no console at all |
| 0004 | propagate `disk_read` failures in MSC | correctness only |

Note the two zephyr pins are only 4 commits apart: our SiWx917 pin is adafruit's
pin plus 4 SiWx917-specific patches, none of which touch Renesas. That is why an
RA8D1 build on the wrong branch still worked.

## 3. Build

```sh
cd ~/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp
make BOARD=renesas_ek_ra8d1 > build.log 2>&1
MAKE_EXIT=$?          # capture directly — piping to `tail` masks the status
```

If it fails at `sysbuild_extensions.cmake:725` on the first run, just run it
again — transient. `rm -rf build-renesas_ek_ra8d1` if it persists.

Artifacts: `build-renesas_ek_ra8d1/zephyr-cp/zephyr/zephyr.{elf,hex,bin}`

## 4. Or use the official prebuilt

<https://circuitpython.org/board/renesas_ek_ra8d1/> — stable 10.2.1, or
10.3.0-alpha.4.

```sh
FW=~/Downloads/adafruit-circuitpython-renesas_ek_ra8d1-en_US-10.3.0-alpha.4.elf
arm-none-eabi-readelf -h "$FW" | grep -i entry     # sanity: 0x20035d9
```

## 5. Flash — use probe-rs, NOT JLinkExe

```sh
brew install probe-rs-tools
probe-rs list
# [0]: J-Link OB -- 1366:1024:000440353787 (SiWx917)
# [1]: J-Link    -- 1366:0105:001086859839 (RA8D1)   <- pin this one
```

probe-rs talks to J-Link probes natively and **never triggers the SEGGER
firmware update** that breaks JLinkExe here (see gotcha below). Read-only
identify first:

```sh
probe-rs info --probe 1366:0105:001086859839      # -> Detected chip: R7FA8D1BH
```

**Strip the option-setting sections before flashing.** probe-rs's R7FA8D1BH
target doesn't map the RA8 Option Function Select region, and fails with:

```
No flash memory contains the entire requested memory range 0x0300A100..0x0300A104
```

Those are 10 sections / 64 bytes total (`.option_setting_ofs0`, `ofs2`,
`dualsel`, `ofs1_sec`, `banksel_sec`, `bps_sec`, `pbps_sec`, `ofs1_sel`,
`banksel_sel`, `bps_sel`) configuring watchdog startup, low-voltage detect,
bank select and **block protection**. Skipping them is the *safer* choice — a
factory-default kit already holds the values Zephyr wants, and mis-writing
block-protection bits is how you permanently lock flash.

```sh
ELF=build-renesas_ek_ra8d1/zephyr-cp/zephyr/zephyr.elf
OUT=/tmp/zephyr-noofs.elf
ARGS=()
for s in $(arm-none-eabi-objdump -h "$ELF" | awk '{print $2}' | grep '^\.option_setting'); do
  ARGS+=(--remove-section "$s")
done
arm-none-eabi-objcopy "${ARGS[@]}" "$ELF" "$OUT"

probe-rs download --chip R7FA8D1BH --probe 1366:0105:001086859839 "$OUT"   # ~23 s
probe-rs reset    --chip R7FA8D1BH --probe 1366:0105:001086859839
```

`probe-rs reset` works over SWD, so it can be issued while `screen` stays open.

## 6. REPL

```sh
screen /dev/cu.usbmodem0010868598391 115200     # ctrl-a k to exit
```

Console reaches the debug probe's VCOM only because of patch 0003. USB CDC
moves no bytes on this board.

## 7. Push files (no CIRCUITPY drive — USB MSC is broken)

```sh
export RA8_CONSOLE=/dev/cu.usbmodem0010868598391   # else it grabs the SiWx917's port
cd ~/Downloads/ada/siwx917/ra8d1-pack
python3 tools/putfile.py demo/code.py /code.py    # ~90 s, sha256 verified
python3 tools/run.py 20                           # soft reset + capture
```

Use **Ctrl-D (soft reset)** when testing filesystem writes. A debugger hard
reset discards CircuitPython's cached dirty blocks and looks exactly like a
flash bug.

---

## ⚠ Gotcha hit on 2026-08-07: J-Link OB firmware auto-update

A plain `connect` made J-Link V8.94 try to upgrade the board's OB firmware
(shipped May 2022 → DLL wants Nov 2025). It timed out mid-update. Probe still
enumerated but its VCOM vanished and the USB product string changed
`J_Link` → `J-Link`.

**Before retrying a connect: USB power-cycle the board.** Retrying first just
re-fires the same update against a half-written probe.

Also check: J-Link OB jumper in default position (per EK-RA8D1 user's manual,
Zephyr's board doc calls this out), and cable in the **J-Link OB / debug** port,
not the USB-FS device port.

## Known scope limits (from Hermes's pack, unverified here)

- Camera scoped but not started — the `camera` feature key is never set by the
  zephyr-cp toolchain, so no board can enable it regardless of devicetree.
- Unverified MIPI-LCD-vs-camera switch conflict.
- The "invalid FAT boot jump breaks macOS mount" claim was retracted; cause of
  that mount failure is still open.

## ⚠ Gotcha 1: touching displayio from the REPL hangs the board

```python
import displayio
displayio.CIRCUITPYTHON_TERMINAL      # <- blocks forever
```

Ctrl-B and Ctrl-C both fail. Only a reset recovers:
`probe-rs reset --chip R7FA8D1BH --probe 1366:0105:001086859839`

Symptom is distinctive: console output stops **mid-print**, truncated
mid-tuple, because the thread blocked while flushing.

**Mechanism** (traced by Hermes 2026-08-07, refined here). `ra_display_write`
has ONE `k_sem_take(frame_buf_sem, K_FOREVER)` at :147, gated on `vsync_wait`,
which two independent paths can set:

```
A  :132  front_buf != l_pend_buf            -> BufferChange  -> vsync_wait = true
B  :142  state != DISPLAY_STATE_DISPLAYING  -> R_GLCDC_Start  -> vsync_wait = true
```

The semaphore's only producer is `renesas_ra_callback_adapter` at :55, giving
only on `DISPLAY_EVENT_LINE_DETECTION`. One producer, one event, no timeout —
which is why Ctrl-C cannot break it.

`FB_NUM` influences **A only**. Counter-intuitively FB_NUM=1 makes the
*partial* path safe (`l_pend_buf` becomes the internal frame buffer, which
already equals `front_buf`), while a *full-screen* write sets
`l_pend_buf = caller's buffer` and can never match, firing A regardless.

**But displayio can never issue a full-screen write**, verified in our tree:

```
py/circuitpy_mpconfig.h:398   CIRCUITPY_DISPLAY_AREA_BUFFER_SIZE = 512 bytes
                              -> 128 words, far under one 480px row
Display.c:311   display_write() per subrectangle, ~107 chunks per full refresh
```

So **trigger A is unreachable from CircuitPython** and both the boot hang and
the REPL wedge are trigger **B** — the display being (re)started. If B is the
only live trigger, bounding that one `k_sem_take` with `K_MSEC(50)` fixes every
case we have and FB_NUM is not load-bearing for us. Untested; worth doing.

Upstream: `#111178` (remove the vsync wait entirely) is the right shape, kills
both triggers, stalled on an API argument. `#110966` is the wrong direction for
us — it makes the wait *more* reliable, so an unfired IRQ hangs *more*
deterministically.

## ⚠ Gotcha 2: a blank white panel with no error is probably YOUR loop

Not the driver. The panel pushes ~3 fps; an animation loop with no `sleep`
re-dirties the framebuffer faster than a refresh can complete and **nothing
ever reaches the glass — no error, no hang, no clue.**

Cost an hour on `acidwarp-pt.py`. Fix was `time.sleep(0.30)` per frame.
Rule out refresh starvation before suspecting GLCDC.

## Display: what actually works

- `demo/code.py` (dashboard) and `demo/acidwarp-pt.py` both render fine.
- Full-screen dirty regions are fine — they still go through the chunked
  partial path.
- Drive the display from `code.py`, not from the REPL.
- Measured ~2.9 fps full-screen. The panel push is the ceiling, not Python.

Open lead (Hermes, plausible not proven): `display_renesas_ra.c` does **zero**
cache maintenance — no `sys_cache_data_flush_range` anywhere — while the frame
buffer sits in SDRAM via `ext-ram = <&sdram1>`. ~107 `memcpy`s per refresh with
D-cache on is a clean mechanism for pixels stranded in cache. Would show as
partial corruption rather than a blank panel.

## Measured on hardware (2026-08-07)

```
CircuitPython  10.3.0-alpha.4-27-g592032da8e
board.board_id renesas_ek_ra8d1
gc.mem_free()  66,066,560     <- 66 MB heap, the 64 MB SDRAM is live
cpu.frequency  480,000,000    <- 480 MHz confirmed
cpu.uid        766c0c433439353553473be9092f4b4e...  (real, non-zero)
os.statvfs("/") 512 B blocks, 238 total, 232 free  <- ~119 KB filesystem
```

Note the split: **66 MB of heap but only ~119 KB of filesystem.** The SDRAM is
RAM, not storage.

`cpu.temperature` and `cpu.voltage` are unimplemented stubs on this port.

## What is on this board (and what is NOT)

**There are ZERO onboard sensors.** Unlike the SiWx917 DK (si7021, veml6035,
IMU, mics), this is a bare eval kit. Every sensor must come from a connector.

| Hardware | In devicetree | Usable from CircuitPython |
|---|---|---|
| MIPI-DSI 480x854 (ILI9806E) | yes | **yes, working** |
| 64 MB SDRAM | yes | **yes, 63 MB heap** |
| 64 MB octo-SPI flash | yes | only ~119 KB as filesystem |
| 3 LEDs, 2 buttons | yes | yes, `digitalio` |
| TRNG | yes | yes, via `os.urandom` |
| Ethernet + PHY | yes | no `socketpool`/`ssl` |
| CAN-FD | yes | no `canio` |
| ADC / DAC / PWM | yes | **no `analogio`/`pwmio`** |
| I3C | yes | no binding |
| Dave2D 2D accelerator | yes | unused by displayio |
| Camera (OV3640, J59) | not enabled | no `camera` module |
| USB HS / FS | yes | CDC+MSC broken |

Expansion: Qwiic (**= STEMMA QT**, best option), 2x Grove, mikroBUS, 2x Pmod,
Arduino UNO R3. Renesas notes Qwiic/Grove "may not be populated" — check yours.

Working CP modules: `busio` `digitalio` `displayio` `vectorio` `terminalio`
`bitmaptools` `jpegio` `gifio` `storage` `sdcardio` `rotaryio` `usb_cdc`
`aesio` `hashlib` `zlib` `msgpack` `random` `math`.

Missing: `board`* `analogio` `pwmio` `touchio` `keypad` `countio` `audio*`
`synthio` `canio` `socketpool` `ssl` `wifi` `rtc` `alarm` `watchdog` `usb_hid`
`neopixel_write` `i2ctarget`.

*`autogen_board_info.toml` says `board = false` but `import board` and
`board.DISPLAY` both work in practice.

**So sensor work = I2C/SPI digital sensors over Qwiic or Pmod.** No analog.

## The $17 chip does NOT give you 66 MB

The RA8D1 has **1 MB on-chip SRAM**. The 64 MB is a *separate SDRAM chip*
soldered to the eval board and wired to the external memory bus:

```c
// generated board/board.c
const uint32_t* const ram_bounds[] = {
    &z_mapped_end, sram0_end,     // ~1 MB internal, mostly used by Zephyr+CP
    &__SDRAM_end,  sdram1_end,    // the external 64 MB part
};
const size_t circuitpy_max_ram_size = 67108864;   // 64 MiB exactly
```

`supervisor/port.c:259` builds a TLSF heap on the first region and
`tlsf_add_pool()`s the rest. Comment at :258: *"Zephyr doesn't maintain one
multi-heap. So, make our own using TLSF."*

Replicating this on a custom board needs RA8D1 (~$17) + an SDRAM chip (~$5-15)
+ a length-matched parallel SDRAM bus layout. The kit is physically huge because
of the SDRAM bus, MIPI + camera connectors, Ethernet, and *five* ecosystem
headers — mostly optionality you are not using.

Kit is $326. Chip ~$17 @100qty, roughly 4-5x an STM32H7.

## Silicon reality check

- **No NPU.** All ML is Helium (MVE) on the CPU. Renesas superseded this part
  18 months later with the RA8P1 + Ethos-U55, quoted at up to 35x inferences.
- M85 scalar uplift over M7 is **6.28 vs 5.29 CoreMark/MHz**, about 19%.
- No MLPerf Tiny result exists for RA8D1. Renesas' own Helium app note
  (R01AN7127) contains no benchmark table.
- **Helium is unreachable from CircuitPython** — no vectorized path in
  zephyr-cp, and no `audio*`/`analogio`/`synthio` here. The feature justifying
  the price premium is invisible from Python.

**Honest pitch: 63 MB of Python heap driving a MIPI panel (~200x a normal
CircuitPython board). Not AI — a cheaper STM32N6 with a real NPU beats it.**

## Why Adafruit has this board

Testbed, not product. zephyr-cp has 26 boards, ~19 vendor eval kits, only 3
actual Adafruit products. Issue
[#9902](https://github.com/adafruit/circuitpython/issues/9902) states the goal:
"minimally support every Zephyr supported platform, reducing the engineering
cost of supporting new platforms." RA8D1 is the board used to build the
devicetree-declared display path, and now the hardest test of the multi-pool
external-RAM heap. Nobody was going to ship a $326 board.

## Nothing we hit is reported upstream

**We do not file upstream.** Findings stay on our forks and in `#the-forge`;
ladyada handles anything that goes public, later. Recorded here as context for
why we are on our own, not as a to-do.

Zero adafruit/circuitpython issues mentioning RA8D1, Renesas, GLCDC or
Cortex-M85. Zero Zephyr issues on the FB_NUM hang. `display_renesas_ra.c` on
main still has bare `K_FOREVER` and Kconfig still defaults FB_NUM to 2, so
anyone building `ek_ra8d1` from main today hits the hang — which also means
there is no upstream fix coming and we carry our own patches indefinitely.

Already in our base (PR #11166, 2026-08-03): RA8 USB queue-depth fixes in
`ek_ra8d1.conf` (`CONFIG_UDC_RENESAS_RA_MAX_QMESSAGES=32` etc.) — tannewt hit
RA8 HS+MSC trouble too.

Also note `_zephyr_disk_write` and `_zephyr_disk_ioctl` discard return values
just like `_zephyr_disk_read` did — patch 0004 only fixed the read.

## Linux on this board

**Native Linux is impossible.** Confirmed across five research angles:

```
upstream arch/arm NOMMU   ARMv7-M ONLY. Zero Armv8-M / Armv8.1-M support.
                          2022 consolidation removed ARMv4/v5; nothing added
                          since. Cortex-M85 is Armv8.1-M.
Cortex-M85 Linux          nothing anywhere. No port, patch series or attempt.
RA8-family Linux          nothing. Renesas FSP offers FreeRTOS, Azure RTOS,
                          Zephyr only.
uClinux precedent         exists for M3/M4/M7. Nothing for M85.
ST's own Cortex-M85       STM32V8 (Nov 2025) also has no Linux.
```

It is deliberate segmentation: Renesas sells RZ/A, RZ/G, RZ/V with Cortex-A and
real MMUs for Linux. RA8 is positioned RTOS-only.

`jserv/cortexm-linux` looks like an on-ramp and is not — Cortex-M4 on **QEMU
MPS2-AN386**, not silicon, self-described as a testbed.

**Emulation is the viable route.** `mini-rv32ima` (~400 lines, ~18 KB, RV32IMA)
gives a *real MMU* inside the guest, which fixes the no-MMU fragmentation that
bites native ports. Measured ports:

```
RP2040   pico-rv32ima   133 MHz   8 MB SPI PSRAM   ~30 s to shell
ESP32-C3 uc-rv32ima     160 MHz   8 MB SPI PSRAM   ~80 s
ESP32-P4                240 MHz  32 MB PSRAM        17.9 s
HPM6750                ~480 MHz   ?                 ~3 s (suspect)
```

Extrapolating to 480 MHz suggests **~10-15 s to a shell**, behaving like a
60-80 MHz native RISC-V (6-8x interpretation overhead). Extrapolation, not
measurement — nobody has run mini-rv32ima on any Cortex-M. Memory bandwidth
binds before clock does.

Our edge: every existing port drives **SPI** PSRAM; this board has **parallel**
SDRAM on the memory bus.

Work in progress under `ra8d1-linux/`. See `notes/`.

## Status

- [x] Board support in-tree (upstream since 2024-12-02, `7f0cc9e7b4`)
- [x] Toolchain verified for Cortex-M85
- [x] Builds clean (`ra8d1/integration`, 4 patches)
- [x] Flash via probe-rs, OFS sections stripped
- [x] REPL live over the probe VCOM
- [x] 63 MB heap / 480 MHz / real UID confirmed on silicon
- [x] **MIPI display rendering** — dashboard + `acidwarp-pt.py`, ~2.9 fps
- [ ] Display hang — mechanism understood (trigger B), `K_MSEC` bound untested
- [ ] USB MSC — broken upstream, not expected to work
- [ ] Emulated Linux — in progress

## ⚠ Gotcha 3: small `flash_write()` to the OSPI NOR fails, large ones succeed

Backwards from intuition and it cost three 6-minute image transfers.

```
flash_erase(nor, 0x40000, 3670016)          OK
flash_write(nor, 0x40010, buf, 4096) x848   OK      all of them
flash_write(nor, 0x40000, &hdr, 16)         FAILS   16B at a 16B-aligned offset
```

**Root cause is a three-layer mismatch, none of it documented:**

```
ek_ra8d1.dts        write-block-size = <1>    advertises "any size is legal"
Zephyr flash_write  no validation             issue #4276 closed Won't Fix,
                                              "drivers check internally" — this
                                              one does not, it delegates to FSP
Renesas FSP         R_OSPI_B_Write(): "without DMAC, write length must be a
                    multiple of CPU access size"  <- the real rejection, inside
                                                     a compiled vendor blob
```

The DTS `write-block-size = <1>` is effectively untrue for this driver. Nothing
at any layer states "you cannot write 16 bytes here."

**Fix — pad the header to its own full aligned block:**

```c
#define IMG_PAYLOAD_OFF 4096U          /* header gets its own block */
memset(buf, 0xFF, sizeof(buf));        /* 0xFF = erased state */
memcpy(buf, &h, sizeof(h));
flash_write(nor, IMG_FLASH_OFF, buf, IMG_PAYLOAD_OFF);
```

This is the *standard* pattern, not a workaround: MCUboot pads all trailer
fields to `BOOT_MAX_ALIGN` with `0xff`, and Zephyr's `stream_flash` pads
automatically. Same footgun in MCUboot #581 and TinyFPGA PR #52. Writing the
header *first* with a separate valid-flag last appears in no production
bootloader — payload first, padded header last is universal.

Unreported anywhere for this driver. Similar unresolved reports exist for
STM32 OSPI (#97078, #79669) and NXP (#64702) with the identical signature.

**Debugging lesson:** print the `flash_write()` return code and
`flash_get_write_block_size()` on the *first* failure. Each blind retry here
cost a full 6-minute transfer.

## Everything stays in the fork

All work lives on `mikeysklar/circuitpython` (and `mikeysklar/zephyr`) branches.
**No upstream PRs, no upstream issues, nothing public-facing.** Fork-internal
PRs are fine. Discussion goes to `#the-forge` with Hermes. ladyada decides what
goes upstream and when — that is not ours to initiate.

Guards already in place on both trees:

```sh
git config remote.pushDefault fork
git remote set-url --push origin NO_PUSH_UPSTREAM_BY_POLICY
```

Verify before any push:

```sh
git remote -v | grep push      # origin must read NO_PUSH_UPSTREAM_BY_POLICY
```
