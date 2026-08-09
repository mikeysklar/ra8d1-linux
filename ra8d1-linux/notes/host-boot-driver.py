#!/usr/bin/env python3
"""Drive mini-rv32ima under a pty: boot, log in as root, run a command, poweroff."""
import os, pty, sys, select, time

emu, img, ram, outfile = sys.argv[1:5]
timeout = float(sys.argv[5]) if len(sys.argv) > 5 else 120.0

pid, fd = pty.fork()
if pid == 0:
    os.execv(emu, [emu, "-f", img, "-m", ram])

buf = b""
start = time.time()
marks = {}
# (needle, response, mark)
steps = [
    (b"buildroot login:", b"root\n", "login_prompt"),
    (b"# ",               b"uname -a; free\n", "shell_prompt"),
    (b"Mem:",             b"poweroff -f\n", "cmd_done"),
]
i = 0
scan_from = 0
while time.time() - start < timeout:
    r, _, _ = select.select([fd], [], [], 0.3)
    if fd in r:
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        buf += d
    if i < len(steps):
        needle, resp, mark = steps[i]
        idx = buf.find(needle, scan_from)
        if idx >= 0:
            marks[mark] = time.time() - start
            scan_from = idx + len(needle)
            time.sleep(0.4)
            os.write(fd, resp)
            i += 1
    elif b"POWEROFF" in buf or b"System halted" in buf or b"reboot: " in buf:
        marks["poweroff"] = time.time() - start
        break

os.close(fd)
try:
    os.kill(pid, 9); os.waitpid(pid, 0)
except Exception:
    pass

open(outfile, "wb").write(buf)
print("=== RAM %s (%d bytes) | image %s ===" % (ram, int(ram, 0), img))
for k in ("login_prompt", "shell_prompt", "cmd_done", "poweroff"):
    print("  %-14s %s" % (k, ("%.2fs" % marks[k]) if k in marks else "NOT REACHED"))
print("  total %.2fs, %d bytes captured" % (time.time() - start, len(buf)))
print("---- tail ----")
sys.stdout.write(buf[-1600:].decode("utf-8", "replace"))
print()
sys.exit(0 if "shell_prompt" in marks else 1)
