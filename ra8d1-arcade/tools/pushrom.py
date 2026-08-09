#!/usr/bin/env python3
"""Push a ROM image to the EK-RA8D1 over the console UART.

Talks to the loader in src/main.c: send 'S', a 4-byte little-endian length,
then the image. The board erases first and prints <RDY> when it is actually
ready to receive, which matters because the erase is long enough to overrun
the UART FIFO.

    ./pushrom.py /dev/tty.usbmodem... image.bin

The image itself carries a CRC, so the board verifies what landed.
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    raise SystemExit("needs pyserial: pip install pyserial")


def wait_for(ser, token, timeout):
    """Read until `token` appears, echoing the board's output as it arrives."""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
            buf += chunk
            if token in buf:
                return True
    return False


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/tty.usbmodem1234")
    ap.add_argument("image", help="image from mkromimage.py or mktestrom.py")
    ap.add_argument("--baud", type=int, default=921600,
                    help="must match app.overlay (default 921600)")
    ap.add_argument("--chunk", type=int, default=1024)
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        data = f.read()
    if data[:8] != b"RA8ARC01":
        raise SystemExit(f"{args.image}: not a RA8ARC01 image")
    print(f"{args.image}: {len(data)} bytes")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)

    ser.write(b"S" + struct.pack("<I", len(data)))
    ser.flush()

    print("waiting for erase...")
    if not wait_for(ser, b"<RDY>", timeout=120):
        raise SystemExit("board never signalled <RDY>")

    sent = 0
    for i in range(0, len(data), args.chunk):
        ser.write(data[i:i + args.chunk])
        ser.flush()
        sent += len(data[i:i + args.chunk])
        print(f"\r  {sent}/{len(data)}", end="", flush=True)
    print()

    # Let the board finish writing and report.
    wait_for(ser, b"LOADER: done", timeout=60)
    deadline = time.time() + 5
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
