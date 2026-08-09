#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EK-RA8D1 rv32 Linux guest port
#
# SPDX-License-Identifier: MIT
"""Bring-up checks for the EK-RA8D1 rv32 Linux guest, in dependency order.

Run this in the guest. Each stage is independent and prints PASS or FAIL, so a
failure tells you which layer broke rather than just that `import board` threw.

    python3 detect_test.py

This is the local equivalent of Adafruit_Python_PlatformDetect's bin/detect.py,
with the guest-specific evidence added in front.
"""

import os
import sys


def stage(name):
    """Decorate a check so it prints PASS/FAIL and never aborts the run."""

    def wrap(func):
        def run():
            print(f"\n--- {name}")
            try:
                func()
            except Exception as err:  # pylint: disable=broad-except
                print(f"FAIL: {type(err).__name__}: {err}")
                return False
            print("PASS")
            return True

        return run

    return wrap


@stage("0. the guest kernel exposes the bridge")
def check_kernel():
    """The two device nodes pv-io.c registers."""
    for path in ("/dev/i2c-0", "/dev/gpiochip0"):
        print(f"  {path}: {'present' if os.path.exists(path) else 'MISSING'}")
    for bus in range(4):
        try:
            with open(
                f"/sys/bus/i2c/devices/i2c-{bus}/name", "r", encoding="utf-8"
            ) as name_file:
                print(f"  i2c-{bus} name: {name_file.read().strip()}")
        except OSError:
            pass
    if not os.path.exists("/dev/i2c-0"):
        raise RuntimeError(
            "no /dev/i2c-0: CONFIG_I2C_CHARDEV, or pv-io.c did not probe"
        )


@stage("1. sysfs GPIO numbering")
def check_gpio_base():
    """What sysfs number line 0 of the paravirt gpiochip actually got."""
    import glob

    found = False
    for chip in sorted(glob.glob("/sys/class/gpio/gpiochip*")):
        with open(f"{chip}/label", "r", encoding="utf-8") as label_file:
            label = label_file.read().strip()
        with open(f"{chip}/base", "r", encoding="utf-8") as base_file:
            base = base_file.read().strip()
        print(f"  {chip}: label={label} base={base}")
        if label == "ra8d1-pv":
            found = True
    if not os.path.isdir("/sys/class/gpio"):
        raise RuntimeError("no /sys/class/gpio: CONFIG_GPIO_SYSFS=y is missing")
    if not found:
        raise RuntimeError("no gpiochip labelled ra8d1-pv")


@stage("2. PlatformDetect")
def check_detect():
    """Chip and board ids, and the one property busio keys off."""
    import adafruit_platformdetect

    detector = adafruit_platformdetect.Detector()
    print(f"  chip id:            {detector.chip.id}")
    print(f"  board id:           {detector.board.id}")
    print(f"  any_embedded_linux: {detector.board.any_embedded_linux}")
    for var in ("BLINKA_FORCECHIP", "BLINKA_FORCEBOARD"):
        if var in os.environ:
            print(f"  ({var}={os.environ[var]} is forcing this)")
    if detector.chip.id is None or detector.board.id is None:
        raise RuntimeError("detection returned None")
    if not detector.board.any_embedded_linux:
        raise RuntimeError(
            "any_embedded_linux is False, so busio.I2C will pick the MicroPython "
            "backend instead of generic_linux. See README.md."
        )


@stage("3. board module")
def check_board():
    """Blinka resolved a board module and it has the I2C aliases."""
    import board

    print(f"  board module: {board.__file__}")
    print(f"  SCL: {board.SCL!r}")
    print(f"  SDA: {board.SDA!r}")
    print(f"  has board.I2C(): {hasattr(board, 'I2C')}")
    if not hasattr(board, "I2C"):
        raise RuntimeError("board.I2C() missing, so SCL/SDA were not both defined")


@stage("4. PureIO straight at /dev/i2c-0")
def check_pureio():
    """Skip Blinka entirely and talk to the char device."""
    from Adafruit_PureIO import smbus

    with smbus.SMBus(0) as bus:
        found = []
        for addr in range(0x08, 0x78):
            try:
                bus.read_byte(addr)
            except OSError:
                continue
            found.append(hex(addr))
    print(f"  read_byte probe found: {found or 'nothing'}")
    print("  (hardware scan previously found a device at 0x14)")


@stage("5. busio.I2C")
def check_busio():
    """The full Blinka path: busio -> generic_linux.i2c -> PureIO -> ioctl."""
    import board

    try:
        i2c = board.I2C()
    except ValueError as err:
        # busio swallows the backend's RuntimeError and reports it as a pin
        # mismatch. See README.md section 7.
        raise RuntimeError(
            f"{err}\n  This usually means /dev/i2c-0 would not open, not that "
            "the pins are wrong. Stage 4 tells you which."
        ) from err
    print(f"  backend: {type(i2c._i2c).__module__}")  # pylint: disable=protected-access
    found = [hex(a) for a in i2c.scan()]
    print(f"  scan: {found or 'nothing'}")
    if len(found) > 100:
        raise RuntimeError(
            "scan reported nearly every address. I2C.scan() tries write_quick() "
            "first, which is a zero-length write; if the bridge returns success "
            "for a zero-length i2c_write() then every address looks present. "
            "See README.md, 'Zero-length writes'."
        )


@stage("6. digitalio on an on-board LED")
def check_digitalio():
    """Blink LED1 three times. Watch the board."""
    import time

    import board
    import digitalio

    led = digitalio.DigitalInOut(board.LED1)
    led.direction = digitalio.Direction.OUTPUT
    for _ in range(3):
        led.value = True
        time.sleep(0.2)
        led.value = False
        time.sleep(0.2)
    print("  blinked LED1 three times")


def main():
    """Run every stage and summarise."""
    stages = [
        check_kernel,
        check_gpio_base,
        check_detect,
        check_board,
        check_pureio,
        check_busio,
        check_digitalio,
    ]
    results = [check() for check in stages]
    print(f"\n{sum(results)}/{len(results)} stages passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
