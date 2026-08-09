#!/usr/bin/env python3
"""Drive mini-rv32ima under a pty and run a scripted list of shell commands.

Generalises notes/host-boot-driver.py. Differences that matter:

  * the image we build auto-logs-in (`login -f root` from inittab) so there is
    no "buildroot login:" to wait for -- we sync on the shell prompt instead;
  * every command is bracketed by a unique echo marker, so a command that
    segfaults is distinguishable from one that produced no output. At 10 MB the
    stock image prints a prompt and *then* fails to exec anything; keying off
    the prompt alone reports a false pass.

A pty is mandatory: the emulator's IsKBHit() latches EOF on a read-only pipe.

usage: host-boot-driver2.py EMU IMAGE RAM OUTFILE [TIMEOUT] [-- CMD [CMD ...]]
exit status: 0 only if every command ran and reported marker + exit code.
"""
import os, pty, sys, select, time, re

argv = sys.argv[1:]
cmds = []
if "--" in argv:
    i = argv.index("--")
    cmds = argv[i + 1:]
    argv = argv[:i]

emu, img, ram, outfile = argv[:4]
timeout = float(argv[4]) if len(argv) > 4 else 120.0
if not cmds:
    cmds = ["uname -a", "free"]

PROMPT = re.compile(rb"(?:^|\n)(?:# |/ #|~ #|[^\n]{0,40}# )$")

pid, fd = pty.fork()
if pid == 0:
    os.execv(emu, [emu, "-f", img, "-m", ram])

buf = b""
start = time.time()
results = []          # (cmd, ran, exit_code)
state = "boot"
sent_at = 0.0
idx = 0
boot_time = None
logged_in = False


def read_some():
    global buf
    r, _, _ = select.select([fd], [], [], 0.2)
    if fd in r:
        try:
            d = os.read(fd, 8192)
        except OSError:
            return False
        if not d:
            return False
        buf += d
    return True


def w(s):
    os.write(fd, s.encode())


alive = True
while alive and time.time() - start < timeout:
    alive = read_some()

    if state == "boot":
        # the stock cnlohr image runs a getty and asks; the image we build
        # auto-logs-in from inittab. Handle both.
        if b"login:" in buf[-120:] and not logged_in:
            logged_in = True
            time.sleep(0.4)
            w("root\n")
            continue
        # any shell prompt, or the login banner having gone by
        tail = buf[-200:]
        if b"# " in tail or b"Welcome to mini-rv32ima" in buf and b"# " in buf:
            boot_time = time.time() - start
            time.sleep(0.5)
            w("\n")
            state = "run"
            sent_at = time.time()
        continue

    if state == "run":
        if idx >= len(cmds):
            w("poweroff -f\n")
            state = "done"
            continue
        if sent_at and time.time() - sent_at > 0.6:
            mark = "MARK%02d" % idx
            # $? is captured by the shell itself; if the binary never execs,
            # the marker still prints but with a non-zero code, and if the
            # shell itself dies neither marker appears.
            w("%s; echo %s_rc=$?\n" % (cmds[idx], mark))
            sent_at = 0.0
        if not sent_at:
            mark = ("MARK%02d_rc=" % idx).encode()
            m = re.search(re.escape(mark) + rb"(\d+)", buf)
            if m:
                results.append((cmds[idx], True, int(m.group(1))))
                idx += 1
                sent_at = time.time()
        continue

    if state == "done":
        if b"POWEROFF" in buf or b"System halted" in buf or b"reboot: " in buf:
            break

os.close(fd)
try:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
except Exception:
    pass

open(outfile, "wb").write(buf)

# anything not reached is a failure, recorded explicitly rather than omitted
while len(results) < len(cmds):
    results.append((cmds[len(results)], False, None))

print("=== RAM %s (%d bytes) | image %s (%d bytes) ===" %
      (ram, int(ram, 0), img, os.path.getsize(img)))
print("  boot to shell : %s" % ("%.2fs" % boot_time if boot_time else "NOT REACHED"))
ok = boot_time is not None
for cmd, ran, rc in results:
    if not ran:
        print("  %-34s NOT REACHED" % cmd[:34])
        ok = False
    else:
        print("  %-34s rc=%d" % (cmd[:34], rc))
print("  total %.2fs, %d bytes captured" % (time.time() - start, len(buf)))
print("---- tail ----")
sys.stdout.write(buf[-2500:].decode("utf-8", "replace"))
print()
sys.exit(0 if ok else 1)
