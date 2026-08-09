#!/usr/bin/env python3
"""Soft-reset the board, capture N seconds of output, then LEAVE IT RUNNING."""
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
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.CRTSCTS
termios.tcsetattr(fd, termios.TCSANOW, a)

def rd(t):
    o = b""; end = time.time() + t
    while time.time() < end:
        try:
            c = os.read(fd, 4096)
            if c: o += c
        except BlockingIOError: pass
        time.sleep(0.02)
    return o

os.write(fd, b"\x02"); time.sleep(0.4); rd(0.8)
for _ in range(10):
    os.write(fd, b"\x03"); time.sleep(0.25)
    if b">>>" in rd(0.5): break
os.write(fd, b"\x02"); time.sleep(0.5); rd(1.0)
os.write(fd, b"\x04")                       # soft reboot -> runs code.py

out = rd(SECS)
os.close(fd)                                # leave board running

txt = out.decode("utf-8", "replace")
print(txt)
print("=" * 60)
print("bytes:", len(out))
for k, v in (("dashboard started", "=== RA8D1 DASHBOARD ==="),
             ("frames advancing", "frame 25"),
             ("EXCEPTION", "Traceback")):
    print("  %-20s %s" % (k, "YES" if v in txt else "no"))
