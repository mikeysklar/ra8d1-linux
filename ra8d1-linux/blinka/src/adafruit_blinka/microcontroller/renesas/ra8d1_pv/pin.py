# SPDX-FileCopyrightText: 2026 EK-RA8D1 rv32 Linux guest port
#
# SPDX-License-Identifier: MIT
"""Renesas RA8D1 pin names, as exposed by the paravirtual I/O bridge.

Names are the RA8D1 port names (P600, P414, ...) so that this file reads like
every other Blinka chip file. What backs them is not an RA8D1 GPIO register but
the guest kernel's gpiochip registered by ra8d1-linux/guest/pv-io.c, which
forwards each access to a real Zephyr gpio_pin_set()/gpio_pin_get() on the
board.

GPIO backend: sysfs, not libgpiod. libgpiod_pin.py imports the `gpiod` CPython
extension at module scope, which is a C build we do not want to add to a
riscv32 Buildroot guest. sysfs_pin.py is stdlib-only. The cost is that the
guest kernel needs CONFIG_GPIO_SYSFS=y in addition to the fragment listed in
notes/05-paravirt-io.md, and sysfs GPIO is deprecated upstream. Switching to
libgpiod later is a one-line change to the import below plus tuple pin ids.

UNTESTED. pv-io.c itself has never been compiled or run.
"""

import glob
import os

from adafruit_blinka.microcontroller.generic_linux.sysfs_pin import Pin

# pv-io.c sets gc.label = "ra8d1-pv" and gc.base = -1, so the kernel picks the
# sysfs numbering dynamically (on a 6.1 kernel with ARCH_NR_GPIOS=512 and no
# other gpiochip that lands at 504, not 0). Rather than hard-code a guess, find
# the chip by label and read its base. Setting gc.base = 0 in pv-io.c would make
# this unnecessary and is the better fix if that file is ever touched again.
_PV_GPIOCHIP_LABEL = "ra8d1-pv"


def _pv_gpio_base() -> int:
    """Return the sysfs GPIO number of line 0 of the paravirt gpiochip."""
    for chip_path in sorted(glob.glob("/sys/class/gpio/gpiochip*")):
        try:
            with open(
                os.path.join(chip_path, "label"), "r", encoding="utf-8"
            ) as label_file:
                if label_file.read().strip() != _PV_GPIOCHIP_LABEL:
                    continue
            with open(
                os.path.join(chip_path, "base"), "r", encoding="utf-8"
            ) as base_file:
                return int(base_file.read().strip())
        except (OSError, ValueError):
            continue
    # Not found: either sysfs GPIO is not compiled in, or we are being imported
    # off-target (docs, linting). Fall back to 0 so the import still succeeds;
    # any actual use will fail loudly at export time.
    return 0


_BASE = _pv_gpio_base()

# Logical pin order is fixed by pv_gpio[] in ra8d1-linux/rvlinux/src/main.c and
# documented in notes/05-paravirt-io.md section 3. Adding a pin there means
# appending a line here.
P600 = Pin(_BASE + 0)  # on-board LED1
P414 = Pin(_BASE + 1)  # on-board LED2
P107 = Pin(_BASE + 2)  # on-board LED3
P009 = Pin(_BASE + 3)  # on-board SW1, active low in DT, reads 1 when pressed
P008 = Pin(_BASE + 4)  # on-board SW2, same
P010 = Pin(_BASE + 5)  # mikroBUS INT
P907 = Pin(_BASE + 6)  # mikroBUS PWM
P507 = Pin(_BASE + 7)  # mikroBUS RST

LED1 = P600
LED2 = P414
LED3 = P107
SW1 = P009
SW2 = P008
MB_INT = P010
MB_PWM = P907
MB_RST = P507


class _BusPin:
    """A pin that leaves the chip on a bus, not through the gpiochip.

    P511 and P512 are wired to the RA8D1's RIIC `iic1` controller and are not
    members of the paravirt gpiochip, so there is no honest integer to give
    sysfs for them. busio.I2C only ever compares these objects by identity
    against the entries in `i2cPorts`, so a marker object is enough. Anyone who
    tries to use one as a DigitalInOut gets a TypeError out of sysfs_pin rather
    than silently toggling the wrong physical pin.
    """

    def __init__(self, name: str) -> None:
        self.id = name

    def __repr__(self) -> str:
        return f"<RA8D1 bus pin {self.id}>"


IIC1_SCL = _BusPin("P512")
IIC1_SDA = _BusPin("P511")

# ordered as i2cId, sclId, sdaId
# Bus 0 is /dev/i2c-0, which pv-io.c registers with i2c_add_numbered_adapter()
# and adap.nr = 0. Physically this is RIIC `iic1` on P512/P511, brought out on
# the MIPI header J58 (and the DVP connector J13). There is no Qwiic/STEMMA
# connector on this board and no pull-ups are fitted for these pins: the sensor
# breakout has to supply them. app.overlay runs the bus at 100 kHz.
i2cPorts = ((0, IIC1_SCL, IIC1_SDA),)

# The bridge has no SPI or UART command, so there are no ports. These are
# defined rather than omitted so that busio.SPI / busio.UART raise a readable
# ValueError instead of an ImportError from `from microcontroller.pin import
# spiPorts` (see Adafruit_Blinka src/busio.py lines 387 and 561).
# ordered as spiId, sckId, mosiId, misoId
spiPorts = ()
# ordered as uartId, txId, rxId
uartPorts = ()
