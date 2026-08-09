#!/usr/bin/env python3
"""Probe the RA8D1 board over the raw REPL for available telemetry sources.
Prints results between sentinels. Read-only, no writes to the FS."""
import os, sys, time, termios, tty

import glob as _glob
# Serial console. Set RA8_CONSOLE, or the first J-Link VCOM is used.
def _find_console():
    p = os.environ.get("RA8_CONSOLE")
    if p:
        return p
    c = sorted(_glob.glob("/dev/cu.usbmodem*"))
    if not c:
        raise SystemExit("no /dev/cu.usbmodem* found; set RA8_CONSOLE")
    return c[0]
PORT = _find_console()
fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.CRTSCTS
termios.tcsetattr(fd, termios.TCSANOW, a)

def rd(t=1.0):
    o = b""; t0 = time.time()
    while time.time() - t0 < t:
        try:
            c = os.read(fd, 4096)
            if c: o += c; t0 = time.time()
        except BlockingIOError: pass
        time.sleep(0.02)
    return o

# friendly repl, stop code.py
os.write(fd, b"\x02"); time.sleep(0.4); rd(0.8)
for _ in range(10):
    os.write(fd, b"\x03"); time.sleep(0.25)
    if b">>>" in rd(0.5): break
os.write(fd, b"\x02"); time.sleep(0.4); rd(1.0)

# raw repl
os.write(fd, b"\x01"); time.sleep(0.8)
banner = rd(1.2)
if b"raw REPL" not in banner:
    print("RAW REPL NOT ENTERED:", banner[-200:]); os.close(fd); sys.exit(1)
time.sleep(0.4); rd(0.4)

def ex(src, wait=6.0):
    os.write(fd, src.encode() + b"\x04")
    o = b""; t0 = time.time()
    while time.time() - t0 < wait:
        try:
            c = os.read(fd, 4096)
            if c: o += c; t0 = time.time()
        except BlockingIOError: pass
        if o.count(b"\x04") >= 2: break
        time.sleep(0.02)
    s = o.decode("utf-8", "replace")
    if s.startswith("OK"): s = s[2:]
    return s.replace("\x04", "").strip()

PROBES = [
 ("help_modules", "help('modules')"),
 ("mcu", "import microcontroller as m; print(m.cpu.frequency, m.cpu.temperature, m.cpu.voltage, m.cpu.reset_reason)"),
 ("uid", "import microcontroller as m; print(''.join('%02x'%b for b in m.cpu.uid))"),
 ("cpus", "import microcontroller as m; print(len(m.cpus))"),
 ("mem", "import gc; gc.collect(); print(gc.mem_free(), gc.mem_alloc())"),
 ("os_uname", "import os; print(os.uname())"),
 ("statvfs", "import os; print(os.statvfs('/'))"),
 ("listdir", "import os; print(os.listdir('/'))"),
 ("board_pins", "import board; print(len(dir(board)))"),
 ("board_list", "import board; print([x for x in dir(board) if not x.startswith('_')])"),
 ("display", "import board; d=board.DISPLAY; print(d.width, d.height, d.rotation, d.brightness)"),
 ("supervisor", "import supervisor; print(supervisor.runtime.run_reason, supervisor.ticks_ms())"),
 ("sdram", "import board,microcontroller; print(hasattr(board,'SDRAM'))"),
 ("time", "import time; print(time.monotonic())"),
]

print("<<<PROBE_BEGIN>>>")
for name, src in PROBES:
    try:
        r = ex(src)
    except Exception as e:
        r = "HOSTERR %s" % e
    print("### %s ###" % name)
    print(r[:1500])
print("<<<PROBE_END>>>")

os.write(fd, b"\x02")
os.close(fd)
