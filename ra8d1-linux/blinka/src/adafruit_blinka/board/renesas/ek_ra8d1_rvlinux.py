# SPDX-FileCopyrightText: 2026 EK-RA8D1 rv32 Linux guest port
#
# SPDX-License-Identifier: MIT
"""Pin definitions for the EK-RA8D1 riscv32 Linux guest.

"Board" here is the Renesas EK-RA8D1 evaluation kit as seen from a riscv32
Linux guest running in an emulator on the board's own RA8D1. Everything below
reaches real silicon through the paravirtual I/O bridge at guest-physical
0x11200000.

The board has no Raspberry Pi style header, so the guide's advice to use D0/D1
header numbering does not apply. Pins are named for what they physically are.
D0..D7 are provided as convenience aliases for the eight logical bridge pins
in their pv_gpio[] order, so that stock CircuitPython examples run unmodified;
they are NOT a header pinout.
"""

from adafruit_blinka.microcontroller.renesas.ra8d1_pv import pin

# On-board, no wiring needed. These are the pins to use for a first smoke test.
LED1 = pin.P600
LED2 = pin.P414
LED3 = pin.P107

# On-board buttons. Active low in devicetree, so pv-io.c already inverts them:
# reading these gives 1 when the button is pressed.
SW1 = pin.P009
SW2 = pin.P008

# mikroBUS signals. These are the only mikroBUS pins not already claimed by an
# enabled pinctrl group (spi1, uart3 and i3c0 take the rest).
MB_INT = pin.P010
MB_PWM = pin.P907
MB_RST = pin.P507

# Convenience aliases in pv_gpio[] index order, not a physical header.
D0 = LED1
D1 = LED2
D2 = LED3
D3 = SW1
D4 = SW2
D5 = MB_INT
D6 = MB_PWM
D7 = MB_RST

# Default I2C. Defining SCL and SDA is what makes src/board.py define the
# board.I2C() helper (Adafruit_Blinka src/board.py lines 69-75).
#
# Physically P512 (SCL) / P511 (SDA) on the MIPI graphics expansion header J58,
# also present on the DVP camera connector J13. There is no Qwiic or STEMMA QT
# connector on this board, no pull-ups are fitted for these pins, and the bus
# runs at 100 kHz. Bring your own pull-ups on the sensor breakout.
SCL = pin.IIC1_SCL
SDA = pin.IIC1_SDA

# No SPI, no UART: the bridge has no command for either, so SCLK/MOSI/MISO are
# deliberately left undefined and board.SPI() will not exist.
