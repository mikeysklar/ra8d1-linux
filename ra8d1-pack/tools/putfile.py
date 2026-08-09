#!/usr/bin/env python3
"""Write a file to the CircuitPython filesystem over the UART-mirrored REPL.

Usage: putfile.py <local_path> <board_path>

Uses the raw REPL (Ctrl-A) for deterministic framing, and sends the file as
base64 in chunks so binary-safe and quoting-proof. Verifies by reading the
file's length and sha back off the board.
"""
import base64
import hashlib
import os
import subprocess
import sys
import termios
import time
import tty

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
# probe-rs selector, e.g. "1366:1024:<serial>". Read `probe-rs list`.
PROBE = os.environ.get("RA8_PROBE", "")
CHUNK = 192

local, remote = sys.argv[1], sys.argv[2]
data = open(local, "rb").read()
sha = hashlib.sha256(data).hexdigest()
print("local %s: %d bytes sha256 %s" % (local, len(data), sha[:16]))

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, a)

    def drain(sec):
        out = b""
        end = time.time() + sec
        while time.time() < end:
            try:
                c = os.read(fd, 4096)
                if c:
                    out += c
                    end = time.time() + 0.25
                else:
                    time.sleep(0.01)
            except BlockingIOError:
                time.sleep(0.01)
        return out

    def send(b, wait=0.12):
        os.write(fd, b)
        time.sleep(wait)

    # Reset so we start from a known state, port held open.
    subprocess.run(["/opt/homebrew/bin/probe-rs", "reset", "--chip",
                    "R7FA8D1BH", "--probe", PROBE],
                   capture_output=True, timeout=60)
    boot = drain(6.0)
    print("boot: %d bytes" % len(boot))

    # Break into the REPL. The running code.py sits in a sleep loop, so a
    # single Ctrl-C can land mid-sleep and be absorbed. Hammer it until the
    # friendly prompt appears.
    got_prompt = False
    for _ in range(10):
        send(b"\x03", 0.25)
        r = drain(0.8)
        if b">>>" in r:
            got_prompt = True
            break
    print("friendly prompt: %s" % got_prompt)
    drain(0.5)

    # Enter RAW repl. Wait for the literal 'raw REPL' string, NOT a bare '>'
    # (the raw-REPL banner itself ends in '>', so waiting on '>' races the
    # mode transition and the board echoes source instead of executing it).
    banner = b""
    for attempt in range(6):
        send(b"\x01", 0.4)
        banner += drain(2.0)
        if b"raw REPL" in banner:
            break
    if b"raw REPL" not in banner:
        print("ABORT: raw REPL never entered: %r" % banner[-300:])
        sys.exit(1)
    print("raw REPL entered (attempt %d)" % (attempt + 1))
    time.sleep(0.6)
    drain(0.4)

    def raw_exec(src, timeout=8.0):
        os.write(fd, src.encode() + b"\x04")
        out = b""
        end = time.time() + timeout
        seen = 0
        while time.time() < end and seen < 2:
            try:
                c = os.read(fd, 4096)
                if c:
                    out += c
                    seen = out.count(b"\x04")
                else:
                    time.sleep(0.01)
            except BlockingIOError:
                time.sleep(0.01)
        return out

    # Remount writable from the board side, then stream the file.
    r = raw_exec("import storage,os\n"
                 "try:\n"
                 "    storage.remount('/', False)\n"
                 "    print('RW ok')\n"
                 "except Exception as e:\n"
                 "    print('RW fail', e)\n")
    print("remount:", r.decode("utf-8", "replace").strip()[:200])

    b64 = base64.b64encode(data).decode()
    raw_exec("import binascii\nf=open(%r,'wb')\n" % remote)
    nchunks = (len(b64) + CHUNK - 1) // CHUNK
    for i in range(nchunks):
        piece = b64[i * CHUNK:(i + 1) * CHUNK]
        # base64 alphabet is A-Za-z0-9+/= -- no quotes, no backslashes -- so a
        # plain single-quoted literal is safe. Assert that rather than trusting
        # repr(), whose escaping bit us at a chunk boundary.
        assert "'" not in piece and "\\" not in piece, \
            "unexpected char in base64 chunk %d" % i
        out = raw_exec("f.write(binascii.a2b_base64('%s'))\n" % piece, 6.0)
        if b"Traceback" in out or b"Error" in out:
            print("CHUNK %d/%d FAILED: %s"
                  % (i + 1, nchunks, out.decode("utf-8", "replace")[:300]))
            sys.exit(1)
        if (i + 1) % 10 == 0 or i + 1 == nchunks:
            print("  chunk %d/%d" % (i + 1, nchunks))
    raw_exec("f.close()\n")

    # Verify on the board. hashlib on this build may lack sha256, so fall back
    # to a checksum computed in pure Python -- still a real content check, not
    # just a length check.
    v = raw_exec("import binascii\n"
                 "d=open(%r,'rb').read()\n"
                 "print('LEN',len(d))\n"
                 "print('CRC',hex(binascii.crc32(d) & 0xffffffff))\n" % remote,
                 15.0)
    vt = v.decode("utf-8", "replace")
    print("verify:", vt.strip()[:300])

    os.write(fd, b"\x02")  # back to friendly REPL
    time.sleep(0.3)

    import zlib
    want_crc = hex(zlib.crc32(data) & 0xffffffff)
    ok_len = ("LEN %d" % len(data)) in vt
    ok_crc = ("CRC %s" % want_crc) in vt
    print()
    print("  expect LEN %d  CRC %s" % (len(data), want_crc))
    if ok_len and ok_crc:
        print("VERDICT: WROTE %s OK (len+crc match)" % remote)
    else:
        print("VERDICT: MISMATCH len_ok=%s crc_ok=%s" % (ok_len, ok_crc))
        sys.exit(1)
finally:
    os.close(fd)
