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

`pip` works, and so does `venv`. What does not exist is **the network**, so
every install is offline, out of the wheelhouse baked into the image at
`/opt/wheels`. All of the below is verified on hardware.

**`--no-deps` is mandatory, and this is the big one.**

```sh
pip3 install --user --no-index --find-links=/opt/wheels --no-deps \
    adafruit-circuitpython-bme280 adafruit-circuitpython-busdevice \
    adafruit-circuitpython-register adafruit-circuitpython-typing
```

Name the CircuitPython dependencies yourself. Without `--no-deps`, pip resolves
the driver's dependency on `adafruit-blinka`, which requires `sysv_ipc` on
Linux — a C extension with no riscv32 wheel in existence:

```
ERROR: Could not find a version that satisfies the requirement sysv_ipc>=1.1.0;
       sys_platform == "linux" and platform_machine != "mips" (from adafruit-blinka)
```

Blinka is already installed in the image, so there is nothing to gain by
letting pip resolve it and everything to lose. On a Pi this never comes up
because `sysv_ipc` has wheels there.

**Pure-Python only.** Most `adafruit-circuitpython-*` drivers qualify. Anything
with a C extension needs a riscv32 wheel — essentially none exist, and the
guest has no compiler. Not a packaging problem you can work around.

**`PYTHONUSERBASE` is preset to `/tmp/py`** in `/etc/profile`, because the
default `~/.local` is on the read-only root and every `--user` install would
fail there.

**venv works**, including pip inside it — `ensurepip` ships its bundled pip and
setuptools wheels, despite `--without-ensurepip` appearing in Buildroot's
`python3.mk`. Use `--system-site-packages`:

```sh
python3 -m venv --system-site-packages /tmp/env
```

That flag is **mandatory**, not stylistic. Blinka, PlatformDetect and PureIO
live in the read-only system `site-packages`; a plain venv hides them and you
cannot reinstall them, because of the `sysv_ipc` wall above. Omit it and you
get an environment where `import board` fails and cannot be repaired.

**Nothing persists.** The root filesystem is read-only and `/tmp` is tmpfs, so
anything installed at runtime is gone on reset. For a library you want to keep,
add it to the wheelhouse and rebuild the image. Persistent on-board installs
need writable storage, which is not built yet.

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
