#!/usr/bin/env python3
"""Validate the dashboard's API assumptions on the board before pushing 8KB."""
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

os.write(fd, b"\x02"); time.sleep(0.4); rd(0.8)
for _ in range(10):
    os.write(fd, b"\x03"); time.sleep(0.25)
    if b">>>" in rd(0.5): break
os.write(fd, b"\x02"); time.sleep(0.4); rd(1.0)
os.write(fd, b"\x01"); time.sleep(0.8)
if b"raw REPL" not in rd(1.2):
    print("NO RAW REPL"); os.close(fd); sys.exit(1)
time.sleep(0.4); rd(0.4)

def ex(src, wait=8.0):
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

CHECKS = [
 # can we build a vectorio rect and mutate its palette + width later?
 ("vectorio", "import displayio,vectorio\n"
              "p=displayio.Palette(1); p[0]=0x112233\n"
              "r=vectorio.Rectangle(pixel_shader=p,width=10,height=10,x=0,y=0)\n"
              "r.width=20; r.pixel_shader[0]=0x445566\n"
              "print('vectorio ok', r.width, hex(r.pixel_shader[0]))"),
 # font sheet + glyph tile_index
 ("fontsheet", "import terminalio\n"
               "f=terminalio.FONT; g=f.get_glyph(ord('A'))\n"
               "print('bmp', f.bitmap.width, f.bitmap.height, 'tile', g.tile_index)"),
 # TileGrid over the font sheet w/ transparent palette
 ("tilegrid", "import displayio,terminalio\n"
              "f=terminalio.FONT; FW,FH=f.get_bounding_box()\n"
              "p=displayio.Palette(2); p[0]=0; p.make_transparent(0); p[1]=0xffffff\n"
              "tg=displayio.TileGrid(f.bitmap,pixel_shader=p,width=8,height=1,tile_width=FW,tile_height=FH,x=0,y=0)\n"
              "tg[0]=f.get_glyph(ord('X')).tile_index\n"
              "print('tilegrid ok', tg.width, FW, FH)"),
 # group scale
 ("groupscale", "import displayio\ng=displayio.Group(scale=2,x=1,y=1); print('group ok', g.scale)"),
 # statvfs indices
 ("statvfs", "import os; s=os.statvfs('/'); print(s, 'bsize',s[0],'blocks',s[2],'bfree',s[3])"),
 # supervisor ticks + run_reason str
 ("sup", "import supervisor; print(str(supervisor.runtime.run_reason).split('.')[-1], supervisor.ticks_ms())"),
 # uid slice
 ("uid", "import microcontroller as m; print(len(m.cpu.uid), ''.join('%02X'%b for b in m.cpu.uid[:16]))"),
 # uname fields used
 ("uname", "import os; u=os.uname(); print(repr(u.sysname), repr(u.release), repr(u.version))"),
 # storage mount attrs
 ("mount", "import storage; m=storage.getmount('/'); print(repr(m.label), m.readonly)"),
 # LED + button simultaneously claimable
 ("io", "import board,digitalio\n"
        "ls=[]\n"
        "for n in ('LED0','LED1','LED2','LED3'):\n"
        "    p=digitalio.DigitalInOut(getattr(board,n)); p.switch_to_output(value=False); ls.append(p)\n"
        "bs=[]\n"
        "for n in ('PUSH_BUTTON_SWITCH_1','PUSH_BUTTON_SWITCH_2'):\n"
        "    b=digitalio.DigitalInOut(getattr(board,n)); b.switch_to_input(digitalio.Pull.UP); bs.append(b)\n"
        "print('io ok', [b.value for b in bs])\n"
        "for p in ls: p.deinit()\n"
        "for b in bs: b.deinit()"),
 # heap headroom for a big group
 ("heap", "import gc; gc.collect(); print('free',gc.mem_free())"),
]

print("<<<CHK_BEGIN>>>")
bad = 0
for n, s in CHECKS:
    r = ex(s)
    ok = "Traceback" not in r
    if not ok: bad += 1
    print("### %-11s %s" % (n, "OK" if ok else "FAIL"))
    print(r[:600])
print("<<<CHK_END>>> failures=%d" % bad)
os.write(fd, b"\x02"); os.close(fd)
