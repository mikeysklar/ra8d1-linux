# RA8D1 work

Renesas EK-RA8D1 (Cortex-M85 @ 480 MHz, 64 MB SDRAM, 480x854 MIPI panel).
Moved here from `siwx917/` on 2026-08-07 to keep it separate from the
SiWx917 port.

## Contents

| dir | what |
|---|---|
| `ra8d1-cli.md` | **start here** — bring-up runbook, 3 documented gotchas, hardware/silicon/Linux findings |
| `ra8d1-linux/` | emulated RISC-V Linux. `rvlinux/` is the Zephyr host app, `notes/` has 7 research writeups, `ra8d-bringup.md` is the record |
| `ra8d1-arcade/` | native Z80 + Pac-Man emulator. Z80 core passes ZEXALL cycle-exact |
| `ra8d1-pack/` | Hermes's CircuitPython bring-up pack + `demo/acidwarp-pt.py` |

## IMPORTANT: the west workspace still lives in siwx917

Builds use the shared Zephyr checkout, which did NOT move:

```sh
source ~/Downloads/ada/siwx917/env.sh
source ~/Downloads/ada/siwx917/.venv/bin/activate
cd ~/Downloads/ada/siwx917/circuitpython/ports/zephyr-cp   # <- west workspace
west build -p always -b ek_ra8d1 ~/Downloads/ada/ra8d1/ra8d1-linux/rvlinux \
  -d ~/Downloads/ada/ra8d1/ra8d1-linux/rvlinux/build
```

`circuitpython/` is a single git repo serving both boards via branches:
`siwx917/integration` and `ra8d1/integration`. It is currently on
`ra8d1/integration`. Splitting it would mean a second full west workspace
(several hundred MB) — not worth it unless the two ports start fighting.

Symlinks were left at the old `siwx917/ra8d1-*` paths so in-flight agents
holding absolute paths keep working. They can be deleted once nothing
references them.

## Board access

```sh
probe-rs list        # RA8D1 is the SEGGER one, 1366:0105:001086859839
                     # 1366:1024:000440353787 is the SiWx917 board - do not touch
screen /dev/cu.usbmodem0010868598391 115200
```

Console is 921600 in `rvlinux/app.overlay`; the board DTS default is 115200.

## State as of the move

- CircuitPython: working, 63 MB heap, MIPI display rendering
- Emulated Linux: working, root shell, 2.4 s boot, image persists in OSPI flash
- Arcade: Z80 core passes ZEXALL, Pac-Man renders on host, 531x headroom there
- In flight: telnet console bridge, Buildroot guest image, GLCDC direct video
