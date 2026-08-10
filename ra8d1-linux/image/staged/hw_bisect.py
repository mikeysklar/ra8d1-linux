#!/usr/bin/env python3
"""Trap-17 bisect on hardware: boot, wait for the conloop prompt, then type.

The rootfs replaces getty/login/ash with /sbin/conloop, a canonical-mode
read-eval loop. If typed commands come back with output, the UART->tty->
process path is good end to end and the fault is ash's raw-mode line editing.
If they do not, canonical mode is broken past boot too and the suspect is the
login/session handoff instead.
"""
import subprocess, sys, time, serial

PORT = "/dev/cu.usbmodem0010868598391"
LOG = sys.argv[1] if len(sys.argv) > 1 else "hw-bisect.log"
BOOT_BUDGET = 18 * 60

CMDS = [
    "echo HW-LOOP-OK",
    "uname -a",
    "ls /dev/i2c-0",
    "i2cdetect -y -r 0",
]

s = serial.Serial(PORT, 921600, timeout=0.5)
s.reset_input_buffer()
subprocess.run(["probe-rs", "reset", "--chip", "R7FA8D1BH",
                "--probe", "1366:0105:001086859839"], capture_output=True)

log = open(LOG, "wb")
buf = b""

def pump(seconds, stop=None, since=0):
    """Read serial into buf; stop early when `stop` appears at/after `since`."""
    global buf
    end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            log.write(d)
            log.flush()
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
            if stop and stop in buf[since:]:
                return True
    return False

# Boot through the autorun tests to the conloop prompt.
if not pump(BOOT_BUDGET, b"===== END TEST ====="):
    print("\n[never saw END TEST]")
    sys.exit(2)
pump(20, b"# ")          # first conloop prompt

print("\n\n=== BISECT: sending commands ===")
verdict = []
for c in CMDS:
    mark = len(buf)
    s.write(c.encode() + b"\n")
    # A command is done when conloop prints its next "# " prompt; 90 s covers
    # i2cdetect at 15 MIPS, echo returns in seconds.
    pump(90, b"# ", since=mark)
    verdict.append((c, len(buf) - mark))
    time.sleep(2)

print("\n=== VERDICT ===")
for c, n in verdict:
    print(f"  {c!r}: {n} bytes of response")
total = sum(n for _, n in verdict)
if b"HW-LOOP-OK" in buf:
    print("RESULT: INPUT WORKS in canonical mode -> fault is raw-mode/line-editing")
elif total == 0:
    print("RESULT: NO INPUT even in canonical mode -> suspect session/handoff or UART rx after boot")
else:
    print("RESULT: partial -- read the log")
log.close()
s.close()
