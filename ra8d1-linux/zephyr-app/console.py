#!/usr/bin/env python3
"""Dump the EK-RA8D1 debug-probe VCOM console.

Usage: console.py [seconds] [port]

Note /dev/cu.usbmodem0004403537871 is a DIFFERENT board (SiWx917); the RA8D1
is 0010868598391.
"""
import os
import sys
import termios
import time
import tty

PORT = "/dev/cu.usbmodem0010868598391"


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    port = sys.argv[2] if len(sys.argv) > 2 else PORT

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)

        deadline = time.time() + secs
        while time.time() < deadline:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
