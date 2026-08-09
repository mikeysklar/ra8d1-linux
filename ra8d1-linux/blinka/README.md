# Blinka support for the EK-RA8D1 riscv32 Linux guest

What PlatformDetect and Blinka each need to accept a new board, what that means
concretely for this target, and the files to do it.

Everything below was checked against upstream source, not against the guide
alone. The guide is from 2020 and was last edited 2024-03-08; Blinka 9.0.0
replaced its central mechanism with a JSON registry, and the guide only
partially caught up. Source read at:

| repo | commit | date |
| --- | --- | --- |
| [adafruit/Adafruit_Blinka](https://github.com/adafruit/Adafruit_Blinka) | `37a4ff53bb76` | 2026-07-30 |
| [adafruit/Adafruit_Python_PlatformDetect](https://github.com/adafruit/Adafruit_Python_PlatformDetect) | `52f61e494c11` | 2026-06-24 |
| [adafruit/Adafruit_Python_PureIO](https://github.com/adafruit/Adafruit_Python_PureIO) | `117f1f19f08a` | 2023-05-25 |

The official process is two guides, not one. The one linked in the task covers
only PlatformDetect and says so on its last page:

- <https://learn.adafruit.com/adding-a-single-board-computer-to-platformdetect-for-blinka?view=all>
  (pages: Overview, Selecting an OS, Environment Setup, Adding the Constants,
  Adding Detection Code, Testing Detection, Next Steps)
- <https://learn.adafruit.com/adding-a-single-board-computer-to-blinka?view=all>
  (pages: Overview, Types of Boards, Software Setup, Adding the Chip File,
  Adding the Board File, Getting GPIO working, Getting I2C Working, Getting SPI
  Working, Getting Serial/UART Working, Adding More Features, Next Steps)

Nothing here has been run. `guest/pv-io.c`, which everything depends on, has
never been compiled.

---

## 1. What each side needs, and how they differ

The one-sentence version, from the Overview page of both guides: **the chip
defines which pins of the microprocessor the GPIO library uses, and the board
defines which physical pin on the board maps to those chip pins.**

The split between repos is different from the split between chip and board.
PlatformDetect answers *what am I running on* and returns two strings.
Blinka takes those two strings and answers *what does `board.SCL` mean*.

### PlatformDetect: two strings, six edits

Constants are plain module-level strings whose value equals their name.

| what | file | example |
| --- | --- | --- |
| chip constant | `adafruit_platformdetect/constants/chips.py` | `A64 = "A64"` |
| board constant | `adafruit_platformdetect/constants/boards.py` | `PINE64 = "PINE64"` |
| board group | same file | `_PINE64_DEV_IDS = (PINE64, PINEBOOK, PINEPHONE)` |
| chip detection | `adafruit_platformdetect/chip.py`, in `Chip._linux_id()` | |
| board detection | `adafruit_platformdetect/board.py`, in the `Board.id` if/elif chain | |
| group property | `adafruit_platformdetect/board.py` | `any_pine64_board` |
| linux flag | `adafruit_platformdetect/board.py`, `any_embedded_linux()` | |

Chip detection runs first because it narrows the board search. `Chip.id`
(`chip.py` lines 44-204) checks environment variables, then dispatches on
`sys.platform`; on Linux it calls `_linux_id()` (lines 208-501), which is a flat
chain of tests returning a `chips.*` constant. The helpers it uses live on
`Detector` in `adafruit_platformdetect/__init__.py`:

| helper | reads | line |
| --- | --- | --- |
| `check_dt_compatible_value(v)` | `/proc/device-tree/compatible` | 51 |
| `get_device_compatible()` | same, raw | 95 |
| `get_device_model()` | `/proc/device-tree/model` | 83 |
| `get_cpuinfo_field(f)` | `/proc/cpuinfo` | 35 |
| `get_armbian_release_field(f)` | `/etc/armbian-release` | 63 |
| `check_board_name_value()` | `/sys/devices/virtual/dmi/id/board_name` | 122 |

Board detection is `Board.id` (`board.py` lines 50-270): one `elif chip_id ==
chips.X` per chip. If a chip has exactly one board it assigns the constant
directly (`elif chip_id == chips.CV1800B: board_id = boards.MILKV_DUO`, line
255). If it has several, it calls a `_vendor_id()` helper defined lower in the
file.

Two things that look optional and are not:

- **The `any_*` group property.** `Board.__getattr__` (line 1454) makes any
  unknown attribute a string compare against `self.id`, so `detector.board.FOO`
  never raises, it just returns False. Real properties like `any_pine64_board`
  (line 1152) compare against a tuple from `constants/boards.py`.
- **`any_embedded_linux()`** (line 1299) is a hand-maintained `yield` list of
  those group properties. Blinka's `busio.I2C.init` (`src/busio.py` line 154)
  branches on it to decide between the Linux I2C backend and the MicroPython
  one. A board missing from this list detects perfectly and then gets the wrong
  I2C implementation. The guide flags this at the end of "Adding Detection
  Code".

### Blinka: two new files, two registry lines

The guide describes a "giant if/else if" in `src/board.py`. That is gone. Since
Blinka 9.0.0 the mapping is data:

| what | file |
| --- | --- |
| chip package | `src/adafruit_blinka/microcontroller/<vendor>/<chip>/{__init__.py,pin.py}` |
| board module | `src/adafruit_blinka/board/<vendor>/<board>.py` |
| chip registry | `src/microcontroller_imports.json` |
| board registry | `src/board_imports.json` |

`src/board.py` (lines 33-44) loads `board_imports.json`, walks it in order, and
`import_mod`s the first module whose key matches. `src/microcontroller/pin.py`,
`src/microcontroller/__init__.py` and `src/digitalio.py` all load
`microcontroller_imports.json` and call `import_microcontroller`
(`src/adafruit_blinka/importing.py` line 92). Order in the JSON is significant:
first match wins.

**The two registries are not symmetric, and this bites.**

```python
# src/board.py line 42
if board_id == getattr(ap_board, board_key):
```

`getattr` on the constants module, with no default. A key in
`board_imports.json` that is not a real constant in
`adafruit_platformdetect.constants.boards` raises `AttributeError` and breaks
`import board` for *every* board, not just yours. So the board constant must
land in PlatformDetect before, or with, the registry line.

```python
# src/adafruit_blinka/importing.py line 105
if getattr(detector.chip, chip_key):
```

That goes through `Chip.__getattr__` (`chip.py` line 503), which returns False
for anything unknown. So a chip key needs no PlatformDetect constant at all.
That asymmetry is what makes the fast path in section 5 possible.

`pin.py` is where the real content is. It defines `Pin` objects named after the
chip, plus three tuples the bus classes import by name:

```python
i2cPorts = ((0, TWI0_SCL, TWI0_SDA),)          # (bus number, scl, sda) -> /dev/i2c-0
spiPorts = ((0, SPI0_SCLK, SPI0_MOSI, SPI0_MISO),)
uartPorts = ((3, UART3_TX, UART3_RX),)
```

`busio.I2C.init` does `from microcontroller.pin import i2cPorts` (line 167) and
finds the port whose `(scl, sda)` are the *same objects* as the ones passed in.
It is identity matching, not pin numbers.

The `Pin` class itself comes from one of the backends in
`src/adafruit_blinka/microcontroller/generic_linux/`:

| backend | needs | notes |
| --- | --- | --- |
| `libgpiod_pin.py` | `gpiod` CPython extension | what most boards use; `import gpiod` at module scope |
| `sysfs_pin.py` | nothing, stdlib only | needs `CONFIG_GPIO_SYSFS=y`; deprecated kernel API |
| `periphery_pin.py` | `python-periphery` | pure Python dependency |
| `rpi_gpio_pin.py`, `lgpio_pin.py` | `RPi.GPIO`, `lgpio` | Pi only |

The board module is thin: import the chip's `pin`, alias names. Defining `SCL`
and `SDA` is what causes `src/board.py` (lines 69-75) to synthesise
`board.I2C()`; defining `SCLK`/`MOSI`/`MISO` does the same for `board.SPI()`.
If you skip them the helper simply does not exist.

---

## 2. The I2C path, verified

The claimed path is right; one detail in it is not.

`busio.I2C` -> `adafruit_blinka.microcontroller.generic_linux.i2c.I2C` ->
`Adafruit_PureIO.smbus.SMBus` -> `/dev/i2c-0`. Confirmed at
`src/busio.py:154-155` and `generic_linux/i2c.py:7,35`.

**PureIO does not use `I2C_RDWR` for the calls Blinka's hot path makes.** It
uses two mechanisms, and which one you get depends on the method
(`Adafruit_PureIO/smbus.py`):

| Blinka call | PureIO method | mechanism | line |
| --- | --- | --- | --- |
| `writeto()` | `write_bytes()` | `ioctl(I2C_SLAVE)` then `file.write()` | 297 |
| `readfrom_into()` | `read_bytes()` | `ioctl(I2C_SLAVE)` then `file.read()` | 164 |
| `writeto_then_readfrom(stop=False)` | `read_i2c_block_data()` | `ioctl(I2C_RDWR)`, 2 messages | 229 |
| `writeto_then_readfrom(stop=True)` | the two above, in sequence | `I2C_SLAVE` twice | `i2c.py:100-103` |
| `scan()` | `write_quick()` then `read_byte()` | `I2C_RDWR` with one **zero-length** message | 269 |

Why it matters for us:

- The plain read and write paths go through the kernel's `i2cdev_read` /
  `i2cdev_write`, which build a single `i2c_msg` and call the adapter's
  `master_xfer`. `pv_i2c_xfer` in `guest/pv-io.c` handles that fine.
- The repeated-START path is the `I2C_RDWR` one, with a write message followed
  by a read message to the same address. That is exactly the pair `pv_i2c_xfer`
  collapses into `PV_CMD_I2C_WRITE_READ` (lines 160-173), so a genuine repeated
  START is issued. This is the case most sensor drivers spend their life in and
  it is the one the driver got right.
- Nothing in this path checks the adapter's functionality mask. See section 2b.

### 2a. No dynamic linking anywhere in the path

Relevant to whether a statically linked or musl interpreter would work.
`Adafruit_PureIO/smbus.py` imports exactly three modules: `ctypes`, `fcntl`,
`struct` (lines 24-27). From `ctypes` it takes only marshalling names —
`c_uint8`, `c_uint16`, `c_uint32`, `cast`, `pointer`, `POINTER`,
`create_string_buffer`, `Structure`. There is no `CDLL`, no `dlopen`, no
`find_library`, no `ctypes.util`. Every bus operation is one
`fcntl.ioctl(fd, I2C_SLAVE | I2C_RDWR, ...)` on a plain file descriptor: six
call sites, lines 154, 188, 215, 264, 285, 383.

One caveat that is not PureIO's doing: `import ctypes` itself runs
`pythonapi = PyDLL(None)` at the bottom of CPython's `ctypes/__init__.py` on
Linux, which is a `dlopen(NULL)`. So the interpreter needs a working `_ctypes`
extension module and a `dlopen` that tolerates a NULL name. That is fine on
glibc, which is what this guest is; a *fully* static musl build is where it
would bite, and it would bite at `import ctypes`, not inside PureIO.

### 2b. What cares about the adapter's capabilities

Checked against Linux v6.1 source, since `/dev/i2c-0` here comes from
`guest/pv-io.c` rather than a real controller.

**Userspace Blinka and PureIO never query it.** `Adafruit_PureIO/smbus.py:45`
defines `I2C_FUNCS = 0x0705` and then never uses it. Grepping `I2C_FUNCS`,
`I2C_FUNC_` and `functionality` across the whole of `adafruit_blinka/`,
`busio.py`, `board.py` and `Adafruit_PureIO/` finds no call site.

**`drivers/i2c/i2c-dev.c` does not gate on it either.** In v6.1 that file
contains zero references to `i2c_check_functionality` or any `I2C_FUNC_*` bit.
`i2cdev_read` and `i2cdev_write` clamp at 8192 bytes and call
`i2c_master_recv`/`i2c_master_send`; `i2cdev_ioctl_rdwr` clamps each message at
8192 and calls `i2c_transfer`. The only requirement is that
`adap->algo->master_xfer` exists, which it does.

So `I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL` at `pv-io.c:200` is not load-bearing for
Blinka. It matters in three other places:

1. **`i2c_check_for_quirks()`** in `drivers/i2c/i2c-core-base.c` — this *is*
   load-bearing, and it is where `pv_i2c_quirks` is enforced:
   `max_write_len`, `max_read_len`, `max_comb_1st_msg_len`,
   `max_comb_2nd_msg_len`, and the `I2C_AQ_*` flags. Quirks and the
   functionality mask are independent; the kernel never derives one from the
   other.
2. **In-kernel SMBus helpers** (`i2c-core-smbus.c`) consult
   `I2C_FUNC_SMBUS_*`. Blinka never reaches them, because PureIO uses `I2C_RDWR`
   and raw read/write rather than the `I2C_SMBUS` ioctl.
3. **busybox's i2c tools**, which are what we would debug with.
   `i2ctransfer` does not check functionality at all, it just issues `I2C_RDWR`,
   so it works regardless. `i2cdetect` calls `I2C_FUNCS` and picks a probe mode:
   `I2C_FUNC_I2C` gets it `ADT_I2C`, which is what we want.
   `i2cget`/`i2cset`/`i2cdump` check individual SMBus bits and exit if one is
   missing (`miscutils/i2c_tools.c` lines 338-428).

**One real inconsistency in `pv-io.c` worth fixing.**
`I2C_FUNC_SMBUS_EMUL` expands to include `I2C_FUNC_SMBUS_QUICK`
(`include/uapi/linux/i2c.h:123-130`), so the driver currently advertises
zero-length-write support. `pv_i2c_quirks.flags` sets only
`I2C_AQ_COMB_WRITE_THEN_READ`, not `I2C_AQ_NO_ZERO_LEN`, so
`i2c_check_for_quirks` lets the zero-length message through to the bridge, and
the zero-length-write ambiguity described above is live.

The clean fix is entirely guest-side, two lines in `pv-io.c`, and better than
the host-side change suggested earlier:

```c
pv->adap.quirks->flags |= I2C_AQ_NO_ZERO_LEN;   /* core returns -EOPNOTSUPP */
return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
```

Both halves are needed. The quirk makes the core reject `write_quick()` with
`-EOPNOTSUPP` before it reaches Zephyr, so PureIO's `scan()` falls through to
its `read_byte()` path (`generic_linux/i2c.py:54-59`) and returns a truthful
result. Dropping `SMBUS_QUICK` from the advertised mask stops the driver
claiming a capability the quirk now refuses, which is what `i2cdetect` reads
when choosing its probe mode.

### Zero-length writes

`I2C.scan()` (`generic_linux/i2c.py:43-61`) calls `write_quick(addr)` first and
only falls back to `read_byte(addr)` on `OSError`. `write_quick` submits one
message with `len = 0` and `buf = NULL`.

That reaches `pv_i2c_xfer` with `m->len == 0`, which sends `PV_CMD_I2C_WRITE`
with `WLEN = 0`, which becomes `i2c_write(dev, buf, 0, addr)` on the Zephyr side
(`rvlinux/src/main.c` line 1086). **Whether that actually probes the bus is
unknown.** If the RA IIC driver short-circuits a zero-length transfer and
returns 0 without addressing anything, `scan()` will report all 112 addresses
as present. `detect_test.py` stage 5 checks for exactly this and says so.

The fix is the two-line `pv-io.c` change in section 2b: declare
`I2C_AQ_NO_ZERO_LEN` and stop advertising `I2C_FUNC_SMBUS_QUICK`. Then the i2c
core rejects the zero-length write before it ever reaches Zephyr and `scan()`
falls back to `read_byte()`. `pv_i2c_quirks` (pv-io.c line 211) currently
declares the 256-byte cap but not `I2C_AQ_NO_ZERO_LEN`, which is why the
message gets through today. Neither file is mine to edit.

---

## 3. How detection should work here, and why

Four candidates were on the table. Taking them honestly:

### rv32 is not the obstacle

Worth stating plainly because it is easy to assume otherwise. PlatformDetect
carries several RISC-V entries — `D1_RISCV`, `C906`, `HFU540`, `JH71X0`,
`JH7110`, `TH1520`, `K1`, `CV1800B` — and every one of them is an rv64 part
(Allwinner D1, SiFive FU540, StarFive JH7100/JH7110, T-Head TH1520, SpaceMiT
K1, Sophgo CV1800B). There is no rv32 entry.

But **PlatformDetect never looks at the ISA or the register width anywhere.**
Grepping `chip.py`, `board.py`, `__init__.py` and `constants/chips.py` for
`riscv`, `rv32`, `rv64`, `xlen` or `isa` returns only constant names and one
docstring; there is no `/proc/cpuinfo` `isa:` parse and no architecture branch.
Detection is entirely device-tree-compatible strings and `/proc/cpuinfo`
`Hardware`/`model name` fields.

So autodetect fails for us because no entry matches this board, not because the
guest is 32-bit, and **a correct rv32 entry looks exactly like a correct rv64
entry**: one string in `constants/chips.py`, one
`check_dt_compatible_value()` call in `_linux_id()`, and the board half. There
is nothing rv32-specific to add. What is 32-bit-specific lives one layer down,
in whether CPython and `_ctypes` build for the guest at all.

**`/proc/cpuinfo` — no.** riscv32 `cpuinfo` has no `Hardware` field, so
`get_cpuinfo_field("Hardware")` returns None and `_linux_id` falls into its
x86 vendor branch, which also returns None. The measured content of this guest
(`notes/05-paravirt-io.md` section 6) is `isa: rv32ima`, `mmu: none` — nothing
board-specific to match on.

**A bespoke environment variable — no.** `BLINKA_FORCECHIP` already exists and
does this properly. A second one would be a private mechanism nobody upstream
would take.

**`/proc/device-tree/compatible` — yes, primary.** We control the guest DTB, so
we can put whatever we like in it, and this is the mechanism every recent
RISC-V addition upstream uses: `sun20i-d1`, `jh7110`, `light-lpi4a`,
`cvitek,cv180x`, `spacemit,k1-x` are all `check_dt_compatible_value` calls in
`chip.py`. Recommended DTB change:

```dts
model = "EK-RA8D1 riscv32 Linux guest";
compatible = "renesas,ek-ra8d1-rvlinux";
```

**The i2c adapter name — yes, as fallback.** `pv-io.c` line 319 names its
adapter `"EK-RA8D1 paravirt I2C"`, which surfaces at
`/sys/bus/i2c/devices/i2c-0/name`. This is the only check that tests the thing
we actually care about (the bridge is live) rather than a label someone typed
into a DTS, and it needs no DTB change at all, so it works on the current image
today.

So the chip detection block is DT compatible, then `/proc/device-tree/model`,
then the adapter probe, in that order. It goes near the top of `_linux_id()` as
the guide advises. The cost is four `open()` calls that ENOENT on any real SBC
before falling through; against the ~60 file reads `_linux_id` already does
(`check_dt_compatible_value` reopens `/proc/device-tree/compatible` every call)
that is noise, but a reviewer may still ask for it to move down.

Board detection needs nothing clever: one chip, one board.

```python
elif chip_id == chips.RA8D1_PV:
    board_id = boards.EK_RA8D1_RVLINUX
```

Caveat worth stating: `/proc/device-tree` requires `CONFIG_OF` and procfs in the
guest. Check with `ls /proc/device-tree/compatible` before relying on the
primary path. The fallback does not care.

### Names chosen

| | value | reasoning |
| --- | --- | --- |
| chip | `RA8D1_PV` | Blinka's "chip" is whatever owns the pins the GPIO library drives. That is the RA8D1, not the emulated rv32 core, which has no pins. `_PV` records that access is via the paravirt bridge. |
| board | `EK_RA8D1_RVLINUX` | the eval kit, running the rv32 Linux guest |
| group | `_RENESAS_EK_IDS` | new; upstream has `renesas/` under `microcontroller/` for RZ/V2H and RZ/V2N but no EK group |
| property | `any_renesas_ek_board` | |

---

## 4. The files

```
blinka/
  README.md
  install.py                      idempotent installer, requires --target and --apply
  detect_test.py                  staged bring-up checks, run in the guest
  src/adafruit_blinka/
    microcontroller/renesas/ra8d1_pv/__init__.py
    microcontroller/renesas/ra8d1_pv/pin.py
    board/renesas/__init__.py
    board/renesas/ek_ra8d1_rvlinux.py
```

The four `src/` files are new files upstream and are complete. The seven
PlatformDetect edits and two JSON registry lines are applied by `install.py`,
which matches on verbatim anchor strings taken from the commits above and
refuses to write if an anchor has moved.

Decisions inside `pin.py` worth knowing about:

**sysfs, not libgpiod.** `libgpiod_pin.py` does `import gpiod` at module scope,
a CPython C extension. Adding that to a riscv32 Buildroot guest is real work.
`sysfs_pin.py` is stdlib only. The price is `CONFIG_GPIO_SYSFS=y` on top of the
fragment in `notes/05-paravirt-io.md` section 7, and that sysfs GPIO is
deprecated. Swapping later is one import line plus tuple pin ids.

**The gpiochip base is resolved at import, not hard-coded.** `pv-io.c` line 334
sets `gc.base = -1`, so the kernel allocates the sysfs numbering dynamically —
on a 6.1 kernel with `ARCH_NR_GPIOS = 512` that lands at 504, not 0. `pin.py`
finds the chip by its label (`ra8d1-pv`, pv-io.c line 331) and reads
`/sys/class/gpio/gpiochipN/base`. Setting `gc.base = 0` in `pv-io.c` would make
that unnecessary and is the better fix if that file is ever edited again.

**`SCL`/`SDA` are marker objects, not `Pin`s.** P512/P511 belong to the RIIC
controller and are not lines on the paravirt gpiochip, so there is no honest
integer to hand sysfs. `busio.I2C` only compares them by identity, so a marker
class is sufficient, and anyone who tries `digitalio.DigitalInOut(board.SCL)`
gets a `TypeError` from `sysfs_pin` instead of quietly toggling an unrelated
pin. This deviates from upstream convention (everyone else uses a real `Pin`)
and a reviewer may object; it is deliberate.

**`spiPorts` and `uartPorts` are empty tuples, not omitted.** The bridge has no
SPI or UART command. `busio.SPI` does `from microcontroller.pin import
spiPorts` (line 387), so omitting them turns a clean `ValueError` into an
`ImportError`.

Physical reality, repeated because it is easy to forget: **there is no Qwiic or
STEMMA connector.** SCL/SDA come out on the MIPI header J58 (and DVP connector
J13) with no pull-ups fitted. The sensor breakout has to supply them. The bus
runs at 100 kHz, dropped from the board's Fast Mode Plus default for that
reason.

---

## 5. `BLINKA_FORCECHIP` and `BLINKA_FORCEBOARD`

Both short-circuit detection completely, and both are read before anything else
in their respective `id` properties.

```python
# chip.py lines 56-60, the first thing Chip.id does
if getattr(os, "environ", None) is not None:
    try:
        return os.environ["BLINKA_FORCECHIP"]
    except KeyError:
        pass
```

```python
# board.py lines 60-63, the first thing Board.id does
try:
    return os.environ["BLINKA_FORCEBOARD"]
except (AttributeError, KeyError):
    pass
```

Three properties of this that matter:

1. **`BLINKA_FORCEBOARD` does not need the chip.** It returns before
   `self.detector.chip.id` is consulted on line 65. Forcing the board alone
   leaves chip detection running normally.
2. **The value is passed through verbatim.** It is never validated against
   `constants/boards.py`. Any string works.
3. **Neither is cached.** Both return before `self._board_id` / `self._chip_id`
   are assigned, so the environment is re-read on every access. Changing the
   variable mid-process changes the answer.

They short-circuit *PlatformDetect*. They do not create a board module. Two
things still have to be true:

- The forced id must have an entry in the relevant Blinka registry, or
  `import board` raises `NotImplementedError: Board not supported <id>`
  (`src/board.py` line 67) and `microcontroller.pin` raises
  `NotImplementedError: Microcontroller not supported` (line 31).
- **`any_embedded_linux` must still be True.** It is computed from `self.id`
  against hard-coded tuples, so a forced id that appears in none of them
  returns False, and `busio.I2C` silently picks
  `generic_micropython.i2c` instead of `generic_linux.i2c`. This is the trap.

### The fastest path to a working demo

`install.py --mode fast` touches **no PlatformDetect source at all**. It works
because of the registry asymmetry from section 1:

- `microcontroller_imports.json` gets `"RA8D1_PV"`, which needs no constant,
  because `import_microcontroller` uses `getattr(detector.chip, key)` and
  `Chip.__getattr__` returns False for unknowns.
- `board_imports.json` cannot take `"EK_RA8D1_RVLINUX"` without the constant —
  `getattr(ap_board, key)` would raise. So it repoints the existing
  `"GENERIC_LINUX_PC"` key at our board module instead. That id is already in
  `any_embedded_linux` via the `generic_linux` property (`board.py` line 1335),
  so `busio.I2C` picks the Linux backend.

```sh
python3 install.py --target /opt/blinka-venv/lib/python3.11/site-packages --mode fast --apply
export BLINKA_FORCECHIP=RA8D1_PV
export BLINKA_FORCEBOARD=GENERIC_LINUX_PC
```

The wrinkle is that this shadows the real `GENERIC_LINUX_PC` mapping, so only do
it in a throwaway venv on the guest. If you would rather keep the honest board
id, the minimum PlatformDetect change is two edits, not seven: the constant in
`constants/boards.py` and one `yield self.id == boards.EK_RA8D1_RVLINUX` line in
`any_embedded_linux()`.

**Note the emulator prerequisite.** None of this runs on the guest described in
`notes/05-paravirt-io.md` section 6, which is nommu with no CPython. It needs
the MMU (Sv32) guest with glibc that other work in this tree is building.

---

## 6. End to end

### Once, on the host

```sh
python3 install.py --target /path/to/guest-rootfs/usr/lib/python3.11/site-packages
# read the dry run, then
python3 install.py --target /path/to/guest-rootfs/usr/lib/python3.11/site-packages --apply
```

`--target` is mandatory and never guessed, and nothing is written without
`--apply`. Point it at the guest rootfs being staged, or at a venv inside the
running guest. Do not point it at a system Python.

### Kernel config the guest needs

On top of the fragment in `notes/05-paravirt-io.md` section 7:

```
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=y      # /dev/i2c-0
CONFIG_GPIOLIB=y
CONFIG_GPIO_CDEV=y        # /dev/gpiochip0
CONFIG_GPIO_SYSFS=y       # additionally needed: sysfs_pin.py uses /sys/class/gpio
```

### In the guest

```sh
pip3 install adafruit-blinka adafruit-platformdetect   # if not baked into the rootfs
python3 detect_test.py
```

`detect_test.py` runs seven stages in dependency order and prints PASS/FAIL per
stage, so a failure names the layer:

0. `/dev/i2c-0` and `/dev/gpiochip0` exist, and the adapter names itself
1. sysfs GPIO numbering, and what base the paravirt chip actually got
2. `Detector().chip.id`, `.board.id`, `.any_embedded_linux`
3. `import board`, and whether `board.I2C()` exists
4. PureIO straight at `/dev/i2c-0`, bypassing Blinka
5. `busio.I2C` and `scan()`, with the zero-length-write check from section 2
6. `digitalio` blinking on-board LED1 three times

Then the real thing. A sensor at 0x14 was found by the hardware scan, so:

```python
import board
i2c = board.I2C()
print([hex(a) for a in i2c.scan()])
```

and with a BME280 on flying leads to J58, pull-ups supplied by the breakout:

```sh
pip3 install adafruit-circuitpython-bme280
```

```python
import board
from adafruit_bme280 import basic as adafruit_bme280

i2c = board.I2C()
bme280 = adafruit_bme280.Adafruit_BME280_I2C(i2c)
print(bme280.temperature)
```

Cheapest test that does not involve Python at all, worth doing first: busybox
`i2ctransfer` is already in the image and drives `/dev/i2c-0` through the same
`I2C_RDWR` ioctl.

### Upstreaming

Per the guide's Next Steps pages: run `black` and `pylint`, then PR
PlatformDetect first (it is the dependency), then Blinka. Note the local
convention that SiWx917-adjacent branches stay on the forks and ladyada
upstreams later, so treat the PR step as optional here.

---

## 7. Troubleshooting

**`board.I2C()` raises `ValueError: No Hardware I2C on (scl,sda)=...`** — this
message is misleading. `busio.I2C.init` (`src/busio.py` lines 169-182) wraps the
backend constructor in `try/except RuntimeError`, and
`generic_linux/i2c.py` line 36 raises exactly that `RuntimeError` when
`/dev/i2c-0` will not open. The swallowed error becomes a `ValueError` about pin
matching. So this message usually means the bus device is missing or
unreadable, not that the pins are wrong. Check `ls -l /dev/i2c-0` and
permissions first. `detect_test.py` stage 4 hits PureIO directly to separate
the two.

**`AttributeError: 'Pin' object has no attribute '_fd'` during garbage
collection** — upstream noise from `sysfs_pin.py` lines 81 and 199. `__del__`
runs even when `__init__` raised, before `_fd` is assigned. It only appears
after a pin has already failed to construct; ignore it and read the real
exception above it.

**`TypeError: Invalid Pin ID, should be integer`** — you passed `board.SCL` or
`board.SDA` to `digitalio`. Those are bus markers, not GPIO lines. Deliberate;
see section 4.

---

## 8. What was verified, and what was not

Run on macOS against clean venvs with the current PyPI releases
(Adafruit-Blinka 9.2.0, Adafruit-PlatformDetect 3.89.1, Adafruit-PureIO 1.1.11)
and separately against the git `main` files in the table at the top:

- `install.py` applies cleanly in both modes, all seven anchors matched, and is
  idempotent (second `--apply` changes nothing).
- Chip and board detect as `RA8D1_PV` / `EK_RA8D1_RVLINUX` via
  `/proc/device-tree/compatible`, and via the i2c adapter name with device-tree
  stubbed out entirely.
- `any_embedded_linux` is True in both cases, so `busio` selects
  `generic_linux.i2c`.
- An unrelated board still detects correctly with the patch applied (RK3588
  checked).
- `import board` resolves to our module; `board.SCL`, `board.SDA`, `board.I2C`
  exist, `board.SPI` correctly does not; `i2cPorts[0]` is
  `(0, board.SCL, board.SDA)` by identity, which is what `busio` matches on.
- Fast mode works with zero PlatformDetect edits: `grep RA8D1_PV` across
  `chip.py`, `board.py` and `constants/chips.py` returns nothing, and
  `import board` still lands on our module.
- The intended failure modes fire: `DigitalInOut(board.SCL)` is a `TypeError`,
  `busio.SPI(...)` is a `ValueError`.
- `black` clean. `pylint` 9.51/10 on the two upstreamable files; the only
  complaints are `invalid-name` on `i2cPorts` / `spiPorts` / `uartPorts`, which
  are named by Blinka's own API and trip the same warning in every existing
  chip file.

Not verified, and not verifiable off the board: opening `/dev/i2c-0`, any
actual bus traffic, and any GPIO. On macOS both stop at the missing device
node, which is the correct behaviour.

---

## 9. Untested

- Anything that needs the guest. Nothing here has run against real hardware.
- `guest/pv-io.c` has never been compiled, so `/dev/i2c-0` has never existed.
- The zero-length-write behaviour behind `I2C.scan()` (section 2).
- Whether `/proc/device-tree` exists in the guest.
- The sysfs gpiochip base, which is why `pin.py` reads it rather than assuming.
- The anchor strings in `install.py` are from the commits in the table at the
  top. A newer PlatformDetect may have moved them; the script will say which.
