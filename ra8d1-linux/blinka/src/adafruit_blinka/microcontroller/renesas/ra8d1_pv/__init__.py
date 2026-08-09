# SPDX-FileCopyrightText: 2026 EK-RA8D1 rv32 Linux guest port
#
# SPDX-License-Identifier: MIT
"""Definition for the Renesas RA8D1 as reached through the paravirtual I/O bridge.

The "chip" here is the RA8D1 (Cortex-M85) running Zephyr. Python does not run on
it. Python runs in a riscv32 Linux guest inside an emulator hosted on that chip,
and reaches the RA8D1's real I2C controller and real port pins through a
register block at guest-physical 0x11200000. See ../../../../../README.md and
ra8d1-linux/notes/05-paravirt-io.md for the register map.
"""
