#!/usr/bin/env python3
"""Push a guest image into one of the EK-RA8D1's two OSPI image slots.

Two transports, one wire format.

    header (both)   'S', slot (1 B), length (4 B LE), crc32 (4 B LE)

TCP is the normal path. The board runs a listener alongside the telnet console
for as long as it is up, so a push lands in flash while the guest keeps
running and takes effect at the next reset. There is no per-block ACK: TCP is
already reliable and already flow controlled, and the board double-buffers so
the socket keeps draining while the flash programs.

    pushimage.py Image.gz --slot kernel --tcp 192.168.1.50
    pushimage.py rootfs.erofs --slot rootfs --tcp 192.168.1.50

UART is the recovery path, for a board whose network never came up or whose
kernel slot is empty. It is much slower -- it ACKs every 4 KB block, which
serialises reception against flash programming -- and it has to be entered at
boot by pressing 'L' at the prompt, which this tool does for you.

    pushimage.py Image.gz --slot kernel            # then reset the board

--dry-run validates the file and prints the crc32 and expected duration for
whichever transport was selected, without touching board or cable.
"""

import argparse
import os
import socket
import sys
import time
import zlib

DEFAULT_UART = "/dev/cu.usbmodem0010868598391"
DEFAULT_TCP_PORT = 5555
BAUD = 921600
BLOCK = 4096                    # UART only; must match the loader's buf[]
ERASE_BLOCK = 256 * 1024
IMG_PAYLOAD_OFF = 4096
OSPI_SIZE = 64 * 1024 * 1024

# Mirrors the slot table in main.c. Over TCP the board announces its own copy
# and this is checked against it before anything is erased; over UART there is
# no banner, so these numbers are all the host has.
KERNEL_SLOT_KB = 8192
SLOTS = {
    "kernel": {"off": 0x40000, "size": KERNEL_SLOT_KB * 1024},
    "rootfs": {"off": 0x40000 + KERNEL_SLOT_KB * 1024,
               "size": OSPI_SIZE - ERASE_BLOCK - (0x40000 + KERNEL_SLOT_KB * 1024)},
}
# The kernel is memcpy'd to guest offset 0 with the DTB and emulator state at
# the top of the 64 MB SDRAM, so it has a second ceiling the rootfs does not.
GUEST_RAM = 64 * 1024 * 1024
MAX_RAM_LEN = GUEST_RAM - 1536 - 48 * 4

# --- timing model -----------------------------------------------------------
# Only the UART wire term is exact. The flash terms come from
# notes/03-image-storage.md ("14 erases of 256 KiB (~7 s) plus ~54k page
# programs (~15 s)" for 3.4 MB): 500 ms per erase, 276 us per 64 B page. The
# TCP throughput is a guess and the single least trustworthy number here --
# the tool reports the rate it actually measured at the end of every run, so
# correct it after the first real push.
BITS_PER_BYTE = 10
# MEASURED on the board, 2026-08-07, from a 24 MB rootfs push: 92 erases in
# 74,885 ms and 375,064 page programs in 304,328 ms. That is 814 ms per 256 KB
# erase and 811 us per 64-byte page -- erase 1.6x and programming 2.9x slower
# than the datasheet-typical figures in notes/03 that these used to carry.
# Cross-checks against the one successful 37.7 MB push: predicts 9.9 min,
# actual was 9m32s.
ERASE_MS_PER_BLOCK = 814.0
PAGE_BYTES = 64                 # the RA OSPI driver's page_size_bytes
PAGE_US = 811.0
TCP_BYTES_PER_S = 1.5 * 1024 * 1024


def human_time(sec):
    sec = int(sec + 0.5)
    return f"{sec}s" if sec < 60 else f"{sec // 60}m{sec % 60:02d}s"


def human_size(n):
    return f"{n / (1024 * 1024):.2f} MB" if n >= 1024 * 1024 else f"{n / 1024:.1f} KB"


def slot_max_len(name):
    cap = SLOTS[name]["size"] - IMG_PAYLOAD_OFF
    if name == "kernel":
        cap = min(cap, MAX_RAM_LEN)
    return cap


def estimate(length, transport, baud):
    """Return a dict of the modelled phase durations, in seconds."""
    erase_blocks = -(-(length + IMG_PAYLOAD_OFF) // ERASE_BLOCK)
    erase = erase_blocks * ERASE_MS_PER_BLOCK / 1000.0
    # +1 block of page programs for the header, written the same way.
    program = (length + IMG_PAYLOAD_OFF) / PAGE_BYTES * PAGE_US / 1e6

    if transport == "uart":
        wire = length * BITS_PER_BYTE / baud
        # The block ACK means reception and programming never overlap.
        total = wire + erase + program
    else:
        wire = length / TCP_BYTES_PER_S
        # Double buffered: the link hides behind the flash, or vice versa.
        total = erase + max(wire, program)

    return {"wire": wire, "erase": erase, "program": program,
            "total": total, "erase_blocks": erase_blocks}


def validate(length, slot):
    if length == 0:
        raise SystemExit("error: image is empty")
    cap = slot_max_len(slot)
    if length > cap:
        raise SystemExit(
            f"error: image is {human_size(length)}, the {slot} slot holds "
            f"{human_size(cap)}")


def progress(done, total, started, prefix):
    el = time.monotonic() - started
    rate = done / el if el > 0 else 0
    eta = (total - done) / rate if rate > 0 else 0
    bar_w = 26
    filled = int(bar_w * done / total)
    sys.stderr.write(
        f"\r{prefix} [{'#' * filled}{'.' * (bar_w - filled)}] "
        f"{100.0 * done / total:5.1f}%  "
        f"{done / (1024 * 1024):6.2f}/{total / (1024 * 1024):.2f} MB  "
        f"{rate / 1024:7.1f} KB/s  elapsed {human_time(el)}  "
        f"eta {human_time(eta)}   ")
    sys.stderr.flush()


# --------------------------------------------------------------------- TCP

class PushFailed(Exception):
    """A push that failed for a reason the board reported.

    `fatal` means retrying cannot help -- the board has told us it needs a
    reset before it will accept anything. Everything else is worth another
    attempt, because the underlying fault is intermittent: the same 37 MB
    payload has both failed four times and succeeded once.
    """

    def __init__(self, msg, fatal=False):
        super().__init__(msg)
        self.fatal = fatal


class TcpBoard:
    def __init__(self, host, port, rx_timeout):
        self.s = socket.create_connection((host, port), timeout=30)
        self.s.settimeout(rx_timeout)
        self.buf = b""

    def close(self):
        self.s.close()

    def line(self, timeout):
        """Read one newline-terminated status line from the board."""
        end = time.monotonic() + timeout
        while b"\n" not in self.buf:
            if time.monotonic() > end:
                raise SystemExit(
                    f"\nerror: timed out after {timeout:.0f}s waiting for the "
                    f"board (last partial: {self.buf!r})")
            self.s.settimeout(min(5.0, max(0.5, end - time.monotonic())))
            try:
                d = self.s.recv(512)
            except socket.timeout:
                continue
            if not d:
                raise SystemExit("\nerror: board closed the connection")
            self.buf += d
        ln, self.buf = self.buf.split(b"\n", 1)
        return ln.decode("utf-8", "replace").strip()

    def read_blkcrc(self, timeout):
        """Collect a BLKCRC ... ENDCRC report. Returns (blocksize, [crc,...])."""
        hdr = self.line(timeout).split()
        if not hdr or hdr[0] != "BLKCRC":
            return None
        blocksize, nblk = int(hdr[1]), int(hdr[2])
        crcs = []
        # Read through ENDCRC unconditionally, not just until nblk CRCs have
        # arrived: stopping on the count leaves the terminator in the buffer
        # and the caller then reads "ENDCRC" where the error line should be.
        while True:
            ln = self.line(timeout)
            if ln == "ENDCRC" or len(crcs) > nblk:
                break
            crcs += [int(t, 16) for t in ln.split()]
        return blocksize, crcs[:nblk]

    def expect_ok(self, what, timeout):
        ln = self.line(timeout)
        if not ln.startswith("OK"):
            # The board says so explicitly when its writer thread had to be
            # abandoned: the OSPI controller is wedged and only a reset clears
            # it, so retrying would just collect identical refusals.
            fatal = "reset the board" in ln or "disabled" in ln
            raise PushFailed(f"board rejected the {what}: {ln}", fatal)
        return ln

    def peek_line(self, timeout):
        """Next line without treating a non-OK as fatal. None on timeout."""
        try:
            return self.line(timeout)
        except SystemExit:
            return None


def tcp_banner_check(board, slot, length):
    """Read and verify the board's slot table before anything is erased.

    The board is the authority on its own layout. If this tool's constants
    disagree with the running firmware, the failure it would cause is erasing
    the wrong region, which no amount of retrying fixes -- so refuse instead.
    """
    ln = board.line(15)
    parts = ln.split()
    if not parts or parts[0] != "RA8LDR":
        raise SystemExit(f"error: not an image loader on that port: {ln!r}")
    if parts[1] != "1":
        raise SystemExit(
            f"error: board speaks loader protocol v{parts[1]}, this tool "
            f"speaks v1 -- update the tool")

    board_slots = {}
    for i, field in enumerate(parts[2:]):
        name, off, cap = field.split(":")
        board_slots[name] = (i, int(off), int(cap))

    print(f"board      {' '.join(f'{n}@0x{o:06x} max {human_size(c)}' for n, (_, o, c) in board_slots.items())}")

    if slot not in board_slots:
        raise SystemExit(
            f"error: board has no {slot!r} slot, only "
            f"{sorted(board_slots)}")

    idx, off, cap = board_slots[slot]
    if off != SLOTS[slot]["off"]:
        raise SystemExit(
            f"error: board puts {slot} at 0x{off:06x}, this tool expects "
            f"0x{SLOTS[slot]['off']:06x} -- firmware and tool disagree, "
            f"refusing to erase")
    if length > cap:
        raise SystemExit(
            f"error: image is {human_size(length)}, board says {slot} holds "
            f"{human_size(cap)}")
    return idx


def report_block_diff(report, data):
    """Diff the board's per-block CRCs against the source we sent.

    The distribution is the finding, not the count. A contiguous tail means
    the writes stopped landing at a point; a scatter means individual flash
    operations are failing independently, which is what an intermittent
    hardware fault looks like; a single bad block in an otherwise perfect
    image is something else again.
    """
    if not report:
        return
    bs, crcs = report
    bad = [i for i, c in enumerate(crcs)
           if (zlib.crc32(data[i * bs:(i + 1) * bs]) & 0xFFFFFFFF) != c]

    print(f"\nblock diff: {len(bad)} of {len(crcs)} {bs // 1024} KB blocks differ "
          f"from what was sent")
    if not bad:
        print("  every block matches -- the payload is intact and the "
              "whole-image CRC or the header is what disagrees")
        return

    # Contiguous runs say much more than a list of indices.
    runs, start, prev = [], bad[0], bad[0]
    for i in bad[1:]:
        if i != prev + 1:
            runs.append((start, prev))
            start = i
        prev = i
    runs.append((start, prev))

    print(f"  first bad block {bad[0]} at payload 0x{bad[0] * bs:x}, "
          f"last {bad[-1]} at 0x{bad[-1] * bs:x}")
    print(f"  {len(runs)} contiguous run(s):", end="")
    for a, b in runs[:12]:
        print(f" [{a}-{b}]" if b > a else f" [{a}]", end="")
    print(" ..." if len(runs) > 12 else "")

    if len(runs) == 1 and runs[0][1] == len(crcs) - 1:
        print("  shape: contiguous tail -- writes stopped landing correctly "
              "partway through")
    elif len(bad) == len(crcs):
        print("  shape: everything wrong -- suspect the base address or the "
              "whole transfer")
    elif len(runs) > len(bad) // 2:
        print("  shape: scattered -- individual flash operations failing "
              "independently, consistent with an intermittent fault")
    else:
        print("  shape: clustered")


def tcp_push(args, data, length, crc, est):
    host, _, port = args.tcp.partition(":")
    port = int(port) if port else DEFAULT_TCP_PORT

    print(f"transport  tcp {host}:{port}")
    board = TcpBoard(host, port, args.rx_timeout)
    try:
        idx = tcp_banner_check(board, args.slot, length)

        t0 = time.monotonic()
        board.s.sendall(b"S" + bytes([idx]) +
                        length.to_bytes(4, "little") + crc.to_bytes(4, "little"))
        board.expect_ok("header", 30)

        print(f"\nerasing {est['erase_blocks']} blocks "
              f"(~{human_time(est['erase'])} modelled)")
        t_erase = time.monotonic()
        board.expect_ok("erase", args.erase_timeout)
        print(f"erase took {human_time(time.monotonic() - t_erase)}")

        # Progress is bytes accepted by the socket, not bytes committed to
        # flash. TCP's window bounds how far ahead that can run, and flash is
        # the slower end, so the two track within a window's worth -- but the
        # last few MB of the bar land before the board has finished writing.
        started = time.monotonic()
        off = 0
        last = 0.0
        chunk = 64 * 1024
        while off < length:
            n = min(chunk, length - off)
            board.s.sendall(data[off:off + n])
            off += n
            now = time.monotonic()
            if now - last > 0.25 or off == length:
                progress(off, length, started, "push")
                last = now
        sys.stderr.write("\n")

        el = time.monotonic() - started
        print(f"payload sent in {human_time(el)} ({length / el / 1024:.1f} KB/s "
              f"into the socket)")

        print("waiting for the board to write the header and verify")
        ln = board.peek_line(args.commit_timeout)
        if ln is None:
            raise PushFailed("no reply to the commit")
        if ln.startswith("BLKCRC"):
            # The board streams a CRC per block before its error line when a
            # verify fails. It has never seen the source, so the comparison
            # has to happen here.
            board.buf = ln.encode() + b"\n" + board.buf
            report_block_diff(board.read_blkcrc(args.commit_timeout), data)
            ln = board.peek_line(30) or "ERR verify failed"
        if not ln.startswith("OK"):
            fatal = "reset the board" in ln or "disabled" in ln
            raise PushFailed(f"board rejected the commit: {ln}", fatal)
        total = time.monotonic() - t0
        print(f"\ndone in {human_time(total)} (modelled {human_time(est['total'])})")
        print(f"measured end-to-end rate {length / total / 1024:.1f} KB/s")
        return 0
    finally:
        board.close()


# -------------------------------------------------------------------- UART

class UartBoard:
    def __init__(self, port, baud):
        import serial   # imported late so --dry-run needs no pyserial

        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.buf = b""

    def close(self):
        self.ser.close()

    def _pump(self):
        d = self.ser.read(4096)
        if d:
            self.buf += d

    def expect(self, needle, timeout, echo=True):
        needle = needle.encode() if isinstance(needle, str) else needle
        end = time.monotonic() + timeout
        shown = 0
        while True:
            i = self.buf.find(needle)
            if i >= 0:
                out, self.buf = self.buf[:i + len(needle)], self.buf[i + len(needle):]
                if echo:
                    sys.stderr.write(out[shown:].decode("utf-8", "replace"))
                    sys.stderr.flush()
                return out
            if echo and len(self.buf) > shown:
                sys.stderr.write(self.buf[shown:].decode("utf-8", "replace"))
                sys.stderr.flush()
                shown = len(self.buf)
            if time.monotonic() > end:
                raise SystemExit(
                    f"\nerror: timed out after {timeout:.0f}s waiting for {needle!r}")
            self._pump()

    def wait_ack(self, timeout):
        end = time.monotonic() + timeout
        while True:
            if self.buf:
                c, self.buf = self.buf[:1], self.buf[1:]
                if c == b"K":
                    return
                if c in b"\r\n":
                    continue        # tail of a line we did not fully consume
                time.sleep(0.3)
                self._pump()
                msg = (c + self.buf).decode("utf-8", "replace").strip()
                raise SystemExit(f"\nerror: board said: {msg}")
            if time.monotonic() > end:
                raise SystemExit(f"\nerror: no ACK for {timeout:.0f}s")
            self._pump()


def uart_push(args, data, length, crc, est):
    idx = list(SLOTS).index(args.slot)

    print(f"transport  uart {args.port} @ {args.baud}")
    board = UartBoard(args.port, args.baud)
    try:
        if args.no_prompt:
            print("\nassuming the board is already in the loader")
        else:
            print(f"\nwaiting up to {args.prompt_timeout:.0f}s for the boot "
                  f"prompt -- reset the board now")
            board.expect("press 'L'", args.prompt_timeout)
            board.ser.write(b"L")
            board.ser.flush()
            # Consume the rest of the banner line so the header echo below does
            # not match its trailing newline instead.
            board.expect("LOADER: waiting for image", 30.0)
            board.expect("\r\n", 5.0)

        t0 = time.monotonic()
        board.ser.write(b"S" + bytes([idx]) +
                        length.to_bytes(4, "little") + crc.to_bytes(4, "little"))
        board.ser.flush()

        # The board echoes what it decoded. Check it agrees before it erases:
        # after the erase the old image is gone.
        board.expect("LOADER: slot=", 30.0)
        seen = board.expect("\r\n", 5.0).decode("utf-8", "replace").strip()
        want = f"{args.slot} len={length} crc={crc:08x}"
        if seen != want:
            raise SystemExit(
                f"\nerror: board decoded the header as {seen!r}, wanted "
                f"{want!r} -- not erasing")

        print(f"\nerasing {est['erase_blocks']} blocks, up to "
              f"{args.erase_timeout:.0f}s allowed")
        t_erase = time.monotonic()
        # Through the trailing newline: the loader writes "\r\n<RDY>\r\n" and
        # the first ACK read would otherwise trip over the leftover CR.
        board.expect("<RDY>\r\n", args.erase_timeout)
        print(f"\nerase took {human_time(time.monotonic() - t_erase)}")

        started = time.monotonic()
        off = 0
        last = 0.0
        while off < length:
            n = min(BLOCK, length - off)
            board.ser.write(data[off:off + n])
            board.ser.flush()
            board.wait_ack(args.ack_timeout)
            off += n
            now = time.monotonic()
            if now - last > 0.25 or off == length:
                progress(off, length, started, "push")
                last = now
        sys.stderr.write("\n")

        el = time.monotonic() - started
        print(f"payload sent in {human_time(el)} ({length / el / 1024:.1f} KB/s, "
              f"{100.0 * est['wire'] / el:.0f}% of line rate)")

        print("waiting for the board to write the header and verify")
        board.expect("LOADER: image committed and verified", args.commit_timeout)
        print(f"\ndone in {human_time(time.monotonic() - t0)} "
              f"(modelled {human_time(est['total'])})")
        return 0
    finally:
        board.close()


# -------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description="Push a guest image into an EK-RA8D1 OSPI image slot.")
    ap.add_argument("image")
    ap.add_argument("--slot", choices=sorted(SLOTS), default="kernel",
                    help="which slot to write (default: %(default)s)")
    ap.add_argument("--tcp", metavar="HOST[:PORT]",
                    help=f"push over TCP instead of the UART "
                         f"(default port {DEFAULT_TCP_PORT})")
    ap.add_argument("-p", "--port", default=DEFAULT_UART,
                    help="serial device for the UART fallback")
    ap.add_argument("-b", "--baud", type=int, default=BAUD)
    ap.add_argument("--dry-run", action="store_true",
                    help="validate the file, print its crc32 and the expected "
                         "duration, and touch neither board nor cable")
    ap.add_argument("--no-prompt", action="store_true",
                    help="uart only: skip waiting for the boot prompt")
    ap.add_argument("--prompt-timeout", type=float, default=120.0)
    ap.add_argument("--erase-timeout", type=float, default=900.0)
    ap.add_argument("--commit-timeout", type=float, default=300.0)
    ap.add_argument("--ack-timeout", type=float, default=30.0,
                    help="uart only: seconds to wait for each block's 'K'")
    ap.add_argument("--rx-timeout", type=float, default=60.0,
                    help="tcp only: socket receive timeout")
    ap.add_argument("--retries", type=int, default=3, metavar="N",
                    help="tcp only: total attempts before giving up "
                         "(default: %(default)s). The push fault is currently "
                         "intermittent rather than deterministic, so a retry "
                         "often succeeds; a failed push has already erased the "
                         "slot, so retrying risks nothing further. Use 1 when "
                         "measuring the failure rate rather than trying to "
                         "land an image.")
    args = ap.parse_args()

    try:
        with open(args.image, "rb") as f:
            data = f.read()
    except OSError as e:
        raise SystemExit(f"error: {e}")

    length = len(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    validate(length, args.slot)

    transport = "tcp" if args.tcp else "uart"
    est = estimate(length, transport, args.baud)
    s = SLOTS[args.slot]

    print(f"image      {os.path.abspath(args.image)}")
    print(f"size       {length} bytes ({human_size(length)})")
    print(f"crc32      0x{crc:08x}")
    print(f"slot       {args.slot} at 0x{s['off']:06x}, "
          f"{human_size(slot_max_len(args.slot))} usable "
          f"({100.0 * length / slot_max_len(args.slot):.1f}% used)")
    print(f"erase      {est['erase_blocks']} x 256 KB")

    if transport == "uart":
        print(f"estimate   {human_time(est['total'])} = "
              f"{human_time(est['wire'])} on the wire at {args.baud} baud "
              f"+ {human_time(est['erase'])} erase "
              f"+ {human_time(est['program'])} flash programming, all serial "
              f"because of the block ACK")
    else:
        print(f"estimate   {human_time(est['total'])} = "
              f"{human_time(est['erase'])} erase then "
              f"max({human_time(est['wire'])} link, "
              f"{human_time(est['program'])} flash programming) overlapped")
        slow = "flash programming" if est["program"] >= est["wire"] else "the link"
        print(f"           bottleneck is {slow}")
    print(f"           (only the uart wire term is exact; erase, programming "
          f"and link rate are modelled)")

    if args.dry_run:
        print("dry run, nothing touched")
        return 0

    if not args.tcp:
        return uart_push(args, data, length, crc, est)

    # Retry, because the fault this works around is intermittent rather than
    # deterministic: the same 37 MB payload has failed four times and passed
    # once, and a failed push has already erased the slot, so a retry costs
    # nothing that is not already lost.
    for attempt in range(1, args.retries + 1):
        try:
            if attempt > 1:
                print(f"\n--- attempt {attempt} of {args.retries} ---")
            return tcp_push(args, data, length, crc, est)
        except (PushFailed, OSError) as e:
            # OSError covers the socket family -- BrokenPipeError when the
            # board resets the connection, TimeoutError when it stops draining
            # -- which are exactly the intermittent faults retrying is for.
            # Before this they escaped the loop and a 3-attempt push died on
            # attempt 1, which is what four "consecutive" failures were.
            print(f"\nattempt {attempt} failed: {type(e).__name__}: {e}")
            if isinstance(e, PushFailed) and e.fatal:
                print("board needs a reset before it will accept a push; "
                      "not retrying")
                return 1
            if attempt == args.retries:
                print(f"giving up after {args.retries} attempts")
                return 1
            time.sleep(2)
    return 1


if __name__ == "__main__":
    sys.exit(main())
