# Using Blinka on the EK-RA8D1: what differs from a Raspberry Pi

For someone who knows Blinka on Pi OS and wants to drive this board. Only the
differences are listed. Anything not mentioned here works the way the Adafruit
guides say it does.

`BUILD.md` is how the image is built; this is how it is used.

**The short version.** Your CircuitPython code is unchanged — `import board`,
`busio.I2C(board.SCL, board.SDA)`, and a scan returns `['0x14']`, exactly as on
a Pi. What changes is everything around the code: it is ~19x slower to start,
the filesystem is read-only, there is no network, and there is no SPI.

---

## 1. Getting a prompt

There is no HDMI, no keyboard, no `ssh`. Two ways in, both giving a root shell
with no password:

```sh
# serial: the J-Link's VCOM, same USB cable as the debugger
screen /dev/cu.usbmodem0010868598391 921600

# telnet: needs the networking build of the app flashed
telnet 192.168.2.3
```

Note **921600**, not the 115200 you may expect. The two consoles are the same
console — the guest's output follows whichever one is attached — so a telnet
client left connected silently steals the serial console. Only one telnet
client at a time.

`ssh` does not exist. It needs an IP stack *inside* the guest, and telnet here
is a host-side relay that the guest sees as a plain UART. See `dropbear` in
`ra8d1-linux/notes/08-guest-net-mmu.md` for what SSH would take.

## 2. Speed: the one thing that will surprise you

The guest runs at **15.37 MIPS**, about 19x slower than a development machine.
It shows up almost entirely in *startup*, not in running code. Measured on the
board:

| | seconds |
|---|---:|
| `python3 -c pass` | 7.34 |
| `python3 -c 'import board'` | 30.35 |
| **second `import board`, same process** | **0.0007** |

A cold `import board` costs 21 s; a warm one is ~30,000x faster. So the entire
cost is per-process, and the fix is not to re-pay it:

```sh
python3 -i myscript.py      # script runs, then you keep the interpreter
```

Import once, then iterate in that session and everything is instant. Running
`python3 myscript.py` in a loop is the single worst way to work on this board.

`BLINKA_FORCECHIP` / `BLINKA_FORCEBOARD` do **not** help — measured 29.63 s vs
30.35 s. They skip detection *logic* while `board.py` still imports all of
PlatformDetect. Detection is automatic here anyway; the bridge advertises
itself and PlatformDetect reads it, so stock examples run unmodified.

## 3. Installing libraries

On Pi OS you make a venv and `pip install`. Here, as shipped, **there is no
`pip`, no `venv`, no `ensurepip`, and no network.** What works instead:

**Pure-Python only.** Most `adafruit-circuitpython-*` drivers qualify. Anything
with a C extension needs a riscv32 wheel — essentially none exist, and there is
no compiler on the guest. This is not a packaging problem you can work around.

**With the packaging build flashed** (`configs/mmu_pip_fragment` plus a
wheelhouse at `/opt/wheels`):

```sh
export PYTHONUSERBASE=/opt/py
pip install --user --no-index --find-links=/opt/wheels adafruit-circuitpython-bme280
```

`PYTHONUSERBASE` is the PEP 370 user site; Python puts it on `sys.path` with no
further setup. It is used here rather than a venv because Buildroot builds
CPython `--without-ensurepip`, so plain `python3 -m venv` cannot bootstrap pip.

**If you do use a venv, `--system-site-packages` is mandatory:**

```sh
python3 -m venv --system-site-packages --without-pip /opt/env
```

Blinka, PlatformDetect and PureIO live in the read-only system
`site-packages`. A plain venv hides them, and you cannot reinstall them — no
network, and `adafruit-blinka` does not build on riscv32. Omitting that flag
gives you an environment where `import board` fails and cannot be repaired.

**Nothing persists.** The root filesystem is read-only and `/tmp` is tmpfs, so
anything installed at runtime is gone on reset. For a library you want to keep,
put it in the rootfs image and push it. Persistent on-board installs need
writable storage, which is not built yet.

## 4. What the hardware actually gives you

| CircuitPython API | works | note |
|---|---|---|
| `board.I2C()`, `busio.I2C` | **yes** | real pins, `SCL=P512` `SDA=P511`, 100 kHz |
| `digitalio.DigitalInOut` | **yes** | 8 GPIOs via `gpiochip504` |
| `busio.SPI` | **no** | the bridge has no SPI; `spiPorts = ()` |
| `pwmio`, `analogio` | **no** | not in the bridge |
| `neopixel` | **no** | needs precise timing the emulator cannot give |

So the canonical `blinkatest.py` from the Adafruit guide gets three of four
steps and then stops:

```
Hello, blinka!
Digital IO ok!
I2C ok!
ValueError: No Hardware SPI on (SCLK, MOSI, MISO)=(None, None, None)
```

That is correct behaviour, not a fault. `board.SCLK`, `MOSI` and `MISO` exist
as names but resolve to `None`. Drop the SPI stanza and the test passes.

Everything I2C goes guest → paravirt bridge at `0x11200000` → Zephyr's RIIC
driver → real silicon. A device really is on the bus; `i2cdetect -y -r 0` and
`board.I2C().scan()` both find `0x14`.

## 5. Smaller differences worth knowing

- **You are already root.** No `sudo`, and nothing to install it.
- **No `raspi-config`, no `raspi-blinka.py`.** I2C is enabled by the devicetree
  overlay at build time; there is nothing to turn on at runtime.
- **Reboot is a reflash-free `probe-rs reset`,** and it takes ~35 s to reach a
  prompt. Resetting also re-enumerates the USB CDC device, so a terminal held
  open across a reset goes deaf — close and reopen it (`BUILD.md` gotcha 21).
- **`/proc/cpuinfo` says riscv32.** Anything sniffing for `BCM` or reading
  `/proc/device-tree/model` to identify a Pi will not recognise this board.
  PlatformDetect already knows: `chip=RA8D1_PV`, `board=EK_RA8D1_RVLINUX`.
- **Timing calls are honest but slow.** `time.monotonic()` works; a `sleep(0)`
  busy loop will not hit the rates a Pi does. Anything bit-banged in Python is
  out.

## 6. A session that works

```sh
telnet 192.168.2.3

# import once, then stay in the interpreter
python3 -i -c "import board, busio; i2c = board.I2C()"

>>> while not i2c.try_lock(): pass
>>> [hex(a) for a in i2c.scan()]
['0x14']
>>> i2c.unlock()
```

The first line costs ~30 s. Everything after it is immediate.
