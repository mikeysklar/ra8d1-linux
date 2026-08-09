# 05 — Paravirtual I/O: real I2C and GPIO from the emulated guest

Goal: let the rv32 Linux guest drive the EK-RA8D1's **actual** I2C bus and GPIO
pins, as the prerequisite for Adafruit Blinka.

Status: **host side implemented and building. Not flashed, not run on hardware.**
Guest side: driver written, never compiled, and — see §7 — blocked behind a
guest rebuild that is already under way in `image/buildroot/`.

Files:

| path | what |
| --- | --- |
| `rvlinux/src/main.c` | host side; the bridge and its self-test |
| `rvlinux/src/main.c.orig` | the known-good 507-line version, preserved untouched |
| `rvlinux/app.overlay` | enables `iic1` |
| `rvlinux/prj.conf` | `CONFIG_I2C=y`, `CONFIG_GPIO=y` |
| `guest/pv-io.c` | guest Linux driver: `/dev/i2c-0` + `/dev/gpiochip0`, **untested** |

---

## 1. Where it sits

`MINIRV32_MMIO_RANGE` is `0x10000000 <= n < 0x12000000`. The guest DTB claims
three addresses in there; the bridge takes a fourth.

```
0x10000000  8250 UART      (existing)
0x11000000  CLINT          (existing)
0x11100000  syscon         (existing)
0x11200000  paravirt I/O   (new, one 4 KB page)
```

Nothing in the DTB describes it, and nothing needs to: a guest that does not
know about the bridge never touches the page. That means `default64mbdtc.h` is
unchanged and the stock cnlohr image still boots exactly as before.

**Hot-path cost is zero.** `MINIRV32_HANDLE_MEM_*_CONTROL` only runs for
addresses the emulator has already determined lie outside guest RAM. Within
those handlers the bridge is the last arm of the existing `if`-chain, behind
the UART and CLINT tests, so ordinary loads and stores never reach it and even
UART traffic pays nothing.

---

## 2. Register map

All offsets from `0x11200000`.

| off | name | dir | meaning |
| --- | --- | --- | --- |
| `0x000` | `ID` | R | `0x50564930` (`'PVI0'`) |
| `0x004` | `VERSION` | R | `1` |
| `0x008` | `CAPS` | R | `[7:0]` I2C buses, `[15:8]` GPIO pins, `[31:16]` DATA bytes |
| `0x00c` | `CMD` | W | write a command code to execute it |
| `0x010` | `STATUS` | R | `0` on success, else a negative Linux errno |
| `0x014` | `RESULT` | R | command-specific; byte count for I2C reads |
| `0x018` | `I2C_BUS` | RW | bus index |
| `0x01c` | `I2C_ADDR` | RW | 7-bit target address |
| `0x020` | `I2C_WLEN` | RW | bytes to send, taken from `DATA` |
| `0x024` | `I2C_RLEN` | RW | bytes to receive, placed in `DATA` |
| `0x028` | `GPIO_PIN` | RW | logical pin index |
| `0x02c` | `GPIO_VAL` | RW | value to write; `GPIO_GET` leaves the reading here |
| `0x030` | `GPIO_FLAGS` | RW | direction/pull bits for `GPIO_CONFIG` |
| `0x200` | `DATA[0..63]` | RW | 64 words = 256 bytes |

Commands:

| code | name | effect |
| --- | --- | --- |
| `0x00` | `NOP` | always succeeds; useful as a liveness check |
| `0x01` | `I2C_WRITE` | send `WLEN` bytes from `DATA` to `ADDR` |
| `0x02` | `I2C_READ` | receive `RLEN` bytes from `ADDR` into `DATA` |
| `0x03` | `I2C_WRITE_READ` | write `WLEN`, repeated START, read `RLEN`; reply lands at `DATA[0]` |
| `0x04` | `I2C_PROBE` | 1-byte read; `STATUS == 0` means the address ACKed |
| `0x10` | `GPIO_CONFIG` | apply `GPIO_FLAGS` to `GPIO_PIN` |
| `0x11` | `GPIO_SET` | drive `GPIO_PIN` to `GPIO_VAL` |
| `0x12` | `GPIO_GET` | read `GPIO_PIN` into `GPIO_VAL` |
| `0x13` | `GPIO_TOGGLE` | invert `GPIO_PIN` |

`GPIO_FLAGS` bits: `0` input, `1` output, `2` pull-up, `3` pull-down,
`4` open-drain, `5` init-high, `6` init-low, `7` disconnect. This is a
deliberately separate little ABI rather than Zephyr's `gpio_flags_t`, so the
guest driver is not coupled to a Zephyr header.

**Commands are synchronous.** The store to `CMD` does not return until the
Zephyr call has finished, so the guest can read `STATUS` on the very next
instruction. There is no busy bit and nothing to poll — no queue, no
completion interrupt, no race. The cost is that the emulator thread blocks for
the duration of a real bus transaction. For a bring-up bridge that is the right
trade; at 100 kHz a 4-byte register read is roughly 400 µs, and the guest's
timebase simply advances by that much, which is truthful rather than a glitch.

### Why every access must be 32-bit

`MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val)` receives the value but **not the
access width**. A byte store and a word store carrying the same low byte are
indistinguishable. The 8250 emulation gets away with this because it only ever
looks at bits 7:0; a data window cannot.

So the `DATA` window is an array of *words*, each packing four payload bytes
little-endian. Host and guest are both little-endian, so on the host side the
word array *is* the byte buffer and no swapping happens anywhere. On the guest
side it means **`memcpy_toio()` and friends are unusable** — the driver must
pack into words explicitly, which `pv_put()`/`pv_get()` in `guest/pv-io.c` do.

(The width *is* recoverable: the macro expands where `ir` is in scope, so
`(ir >> 12) & 7` would give it. I did not do that — it silently depends on a
local variable name in vendored third-party code, and the 32-bit-only rule
costs the guest driver about ten lines.)

---

## 3. What is behind it, on the board

**I2C — one bus, `iic1`.** The RIIC controller on **P512 (SCL) / P511 (SDA)**.
The board DTS wires its pinctrl but leaves it `disabled`; `app.overlay` now
marks it `okay` at 100 kHz, following the same pattern as the Renesas LCD
shield overlays in `boards/shields/rtkmipilcdb00000be/`.

Be aware what those pins physically are: **the EK-RA8D1 has no Qwiic or Grove
connector.** P511/P512 come out on the MIPI graphics expansion header (J58) and
the DVP camera connector (J13). Attaching a sensor means flying leads, and the
pull-ups have to come from the sensor breakout — the board does not fit them
for these pins. I dropped the rate from the board's Fast Mode Plus default to
100 kHz for that reason.

The other candidate was `i2c4` (SCI4 in simple-IIC mode) on the mikroBUS header
at P400/P401, which would be mechanically much nicer. It is unusable: those two
pins are already claimed by `i3c0`, which the board DTS has `okay`.

**GPIO — eight logical pins.**

| idx | name | pin | note |
| --- | --- | --- | --- |
| 0 | LED1 | P600 | on-board, no wiring |
| 1 | LED2 | P414 | on-board |
| 2 | LED3 | P107 | on-board |
| 3 | SW1 | P009 | on-board button, active-low + pull-up from DT |
| 4 | SW2 | P008 | on-board button |
| 5 | MB_INT | P010 | mikroBUS INT |
| 6 | MB_PWM | P907 | mikroBUS PWM |
| 7 | MB_RST | P507 | mikroBUS RST |

Pins 5–7 are the only mikroBUS signals not already claimed by an enabled
pinctrl group — I checked each against `ek_ra8d1-pinctrl.dtsi`. The mikroBUS
SPI pins belong to `spi1`, the UART pins to `uart3`, and the I2C pins to
`i3c0`, all of which are `okay` in the board DTS.

Specs come from devicetree (`GPIO_DT_SPEC_GET`), so active-low is handled by
the driver: `GPIO_GET` on a button returns **1 when pressed**. Adding a pin
means appending one row to `pv_gpio[]` and one to `pv_gpio_name[]`.

---

## 4. Self-test

Since flashing is not allowed here, the host runs a self-test at startup,
before the guest boots, and prints the result to the console. It goes through
`mmio_store()`/`mmio_load()` rather than calling the register handlers
directly, so the address decode, the register file and the Zephyr driver calls
are all exercised on exactly the path the guest will take. A pass leaves only
the guest's own access width unproven.

It checks:

1. `ID`/`VERSION`/`CAPS` read back correctly — proves the decode fires.
2. Register and `DATA` round-trip, including that a neighbouring word is not
   disturbed and that a write to the read-only `ID` is dropped rather than
   aliased onto something else.
3. Every GPIO: configure, and for the LEDs set high, read back, set low.
   The three LEDs are then held on for 150 ms and switched off, so a human
   watching the board gets an unambiguous "the bridge reached real hardware"
   signal independent of the console.
4. An I2C scan of `0x08`–`0x77`, printing every address that ACKs.
5. An unknown command is rejected with `-ENOTSUP` rather than silently ignored.

The scan is bounded by a **3 s deadline**. If no pull-ups are fitted — the
likely case with nothing attached — every address costs a hardware timeout, and
this must not turn a 2.4 s boot into a minute of silence. It reports how many
addresses it got through and how long that took, so the timing is visible
rather than guessed.

Expected output with nothing wired to J58:

```
--- paravirt I/O selftest @ 0x11200000 ---
id 0x50564930 ok, ver 1, i2c buses 1, gpio pins 8, data 256 B
regs+data round-trip ok
  gpio 0 LED1 out cfg ok read 1 ok
  ...
  gpio 3 SW1  in  cfg ok read 0 ok
  ...
  i2c scan 0x08-0x77: none (112 probed in NNN ms)
--- selftest PASS ---
```

`NNN` is the number I most want to see on the first flash. If it is large, the
deadline needs lowering or the scan needs to move behind a flag.

---

## 5. Build

```sh
source /Users/sklarm/Downloads/ada/siwx917/env.sh
source /Users/sklarm/Downloads/ada/siwx917/.venv/bin/activate
cd /Users/sklarm/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp
west build -p always -b ek_ra8d1 \
  /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/rvlinux \
  -d /Users/sklarm/Downloads/ada/siwx917/ra8d1-linux/rvlinux/build
```

Clean, no warnings from `main.c`.

```
FLASH:  60640 B / 2016 KB   2.94%
RAM:    20160 B /  896 KB   2.20%
```

Verified in the build output rather than assumed:

- `CONFIG_I2C_RENESAS_RA_IIC=y` and `CONFIG_GPIO_RA_IOPORT=y` in `.config`
- `DT_N_S_soc_S_iic1_4025e100_STATUS_okay 1`, `clock_frequency 100000`
- `i2c_ra_iic_transfer`, `i2c_ra_iic_config_0`, `pv_exec`, `pv_data` all in the ELF
- the self-test strings survive `-O2` (it is inlined into `main`, not stripped)

**Not flashed.** Another agent may be on the board and the user is watching the
console. Flashing is pending.

---

## 6. What the guest actually is — measured, not assumed

I booted the current image under the host emulator
(`emulator/mini-rv32ima/mini-rv32ima`, 23 s including a full command batch) and
inspected the running system. This is the evidence the rest of the assessment
rests on.

```
Linux buildroot 6.1.14 #4  riscv32 GNU/Linux
/proc/cpuinfo:  isa: rv32ima    mmu: none
/proc/iomem:    10000000-100000ff : serial
                80000000-83ffefff : System RAM        (nothing else)
```

| thing | present? | how I know |
| --- | --- | --- |
| `/dev/mem`, `/dev/kmem` | **no** | `ls -l` → No such file. `CONFIG_DEVMEM` is off |
| loadable modules | **no** | no `/proc/modules`, no `/lib/modules`, no `/proc/kallsyms` |
| kernel I2C subsystem | **no** | no `i2c` strings in the kernel; only busybox's own |
| `/proc/config.gz` | no | not built in |
| Python / perl / cc / gcc | **no** | `which` returns nothing; busybox-only userspace |
| busybox `i2ctransfer` | **yes** | in the applet list; opens `/dev/i2c-%d`, uses `I2C_RDWR` |

And on the emulator itself: `mini-rv32ima.h` implements **no `satp` CSR, no
page-table walk, and no supervisor-mode CSRs at all** — the CSR switch handles
only the machine-mode set (`0x300`–`0x344`). It is a machine-mode-only,
nommu-only core.

---

## 7. The guest-side path to `/dev/i2c-0` — clear-eyed

### The `/dev/mem` idea does not survive contact

There is no `/dev/mem` in this kernel, so there is nothing to `mmap`. Worth
noting the reason it looked attractive is real though: this guest is nommu and
prints *"This architecture does not have kernel memory protection"* at boot,
everything runs in machine mode, and the emulator sets up no PMP. A userspace
process could very likely just dereference `0x11200000` as a plain volatile
pointer and hit the bridge, no `/dev/mem` needed at all.

I could not test that, and it is moot anyway: there is no compiler in the guest
and no `devmem` busybox applet, so *any* userspace test program has to be
cross-compiled — which means touching the Buildroot tree — and once you are
rebuilding, the in-kernel route is strictly better. **Recommendation: do not
pursue `/dev/mem`.**

### The out-of-tree module idea is not merely hard, it is impossible

`CONFIG_MODULES=n`. There is no module loader in this kernel binary to insmod
into. An out-of-tree module is not a difficulty to be assessed, it is a
non-starter until the kernel is rebuilt.

### The realistic path: build the driver into a rebuilt kernel

This is the one that works, and it is **less** work than either alternative
because the rebuild is already happening — `image/buildroot/` was created by
another agent this session and stages cnlohr's configs with fragments layered
on top for networking. The bridge needs one more fragment and one more file:

```
# kernel_fragment
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=y      # this is what creates /dev/i2c-0
CONFIG_GPIOLIB=y
CONFIG_GPIO_CDEV=y        # /dev/gpiochip0
```

plus `guest/pv-io.c` dropped in as e.g. `drivers/i2c/busses/i2c-ra8d1-pv.c`
with an unconditional `obj-y`. It registers an `i2c_adapter` whose
`master_xfer` pokes the registers above, and a `gpio_chip` alongside it. It
collapses the write-then-read message pair into the single `I2C_WRITE_READ`
command so the bridge issues a genuine repeated START, which many devices
require, and it declares `i2c_adapter_quirks` for the 256-byte window so the
i2c core splits or rejects oversized transfers instead of letting them fail
deep in the bridge.

It is **written but never compiled and never run.** Treat it as a starting
point.

The nice property of this route: once `/dev/i2c-0` exists, **busybox's
`i2ctransfer` already in the image can drive it with no new userspace at all.**
That is the cheapest possible end-to-end test — no Python, no cross-compiling,
one command at the guest shell.

### Blinka: say it plainly

**Blinka cannot run on this guest, and I2C is not the reason.**

Blinka needs CPython 3. This guest is **nommu**, and CPython on nommu is not a
practical target — Buildroot itself gates `python3` behind `BR2_USE_MMU`. So
the blocker is the interpreter, not the bus.

And the obvious fix — build an MMU (Sv32) guest instead — is blocked one level
further down: **the emulator has no MMU.** No `satp`, no page-table walk, no
supervisor mode. Getting Blinka would mean adding Sv32 to `mini-rv32ima.h` and
then rebuilding for an MMU kernel with a full glibc/musl userspace, on a board
with 64 MB of SDRAM running an interpreter at roughly 2 BogoMIPS. That is a
different and much larger project than this one.

So, honestly:

| goal | reachable? |
| --- | --- |
| guest reads/writes real I2C devices | **yes** — kernel rebuild + `guest/pv-io.c` |
| `/dev/i2c-0` behaving like standard i2c-dev | **yes** — that is exactly what the driver registers |
| busybox `i2ctransfer` driving a real sensor | **yes**, and it needs no new userspace |
| a C program using the i2c-dev ioctls | **yes**, cross-compiled from the Buildroot toolchain |
| **Blinka / CircuitPython libraries** | **no** — needs CPython, needs an MMU, needs an emulator that has one |

"How close can we get to Linux i2c-dev" — all the way. The adapter is a real
`i2c_adapter`; `/dev/i2c-0` gets the standard `I2C_RDWR` and `I2C_FUNC_*`
semantics, and `I2C_FUNC_SMBUS_EMUL` means the SMBus helpers work on top. The
only deviations are the 256-byte transfer cap (declared through the quirks
mechanism, so callers see it properly) and no 10-bit addressing.

---

## 8. Not verified without hardware

Everything below is untested and should be treated as such:

- The whole thing on real silicon. **Never flashed.** The self-test result is a
  prediction.
- Whether `iic1` initialises at all — the board has never had this controller
  enabled. If it fails, the self-test prints `-ENODEV` per command rather than
  hanging.
- I2C scan timing with no pull-ups fitted. The 3 s deadline is a guess at a
  safe bound, not a measurement.
- Whether P010 / P907 / P507 are truly free. I checked them against every
  pinctrl group in `ek_ra8d1-pinctrl.dtsi`, but not against the board
  schematic.
- **The guest half in its entirety.** `guest/pv-io.c` has never been compiled.
- The 32-bit access assumption from the guest side. `writel`/`readl` on riscv
  emit `sw`/`lw`, so it should hold, but it has not been observed.

## 9. Suggested next steps

1. Flash and read the self-test output. That alone settles §8's first three
   items and costs one boot.
2. Add the bridge to the **host** emulator (`emulator/mini-rv32ima/`) with a
   fake I2C backend. Then `guest/pv-io.c` can be developed and debugged at full
   host speed with no board in the loop. I did not do this — it is outside what
   was asked and touches a file I was not given — but it is cheap and it would
   de-risk the whole guest side.
3. Fold the kernel fragment and `pv-io.c` into the Buildroot build already in
   flight, then test with `i2ctransfer` against a real sensor on J58.
