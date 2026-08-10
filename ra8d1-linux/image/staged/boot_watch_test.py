#!/usr/bin/env python3
"""Reset the board and capture the boot log until the autorun test finishes.

Nothing is ever typed at the guest: the S99i2ctest script prints everything.
Exits 0 when '===== END TEST =====' is seen, 1 on timeout. The guest runs at
~15 MIPS and imports CPython packages, so the overall budget is large.
"""
import subprocess, sys, time, serial

PORT = "/dev/cu.usbmodem0010868598391"
LOG = sys.argv[1] if len(sys.argv) > 1 else "boot-blinka.log"
BUDGET = 20 * 60          # whole-boot budget, seconds
QUIET_ABORT = 6 * 60      # give up if the console goes silent this long

s = serial.Serial(PORT, 921600, timeout=0.5)
s.reset_input_buffer()

subprocess.run(["probe-rs", "reset", "--chip", "R7FA8D1BH",
                "--probe", "1366:0105:001086859839"], capture_output=True)

buf = b""
last_data = time.time()
end = time.time() + BUDGET
done = False
with open(LOG, "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            f.write(d)
            f.flush()
            last_data = time.time()
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
            if b"===== END TEST =====" in buf:
                done = True
                # tail: catch anything queued right after the marker
                t = time.time() + 5
                while time.time() < t:
                    d = s.read(4096)
                    if d:
                        f.write(d)
                        sys.stdout.write(d.decode("utf-8", "replace"))
                break
        elif time.time() - last_data > QUIET_ABORT:
            print(f"\n[console silent for {QUIET_ABORT}s, giving up]")
            break

s.close()
sys.exit(0 if done else 1)
