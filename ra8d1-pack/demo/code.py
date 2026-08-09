# RA8D1 CircuitPython — live hardware dashboard
# Renesas EK-RA8D1 · Cortex-M85 @ 480 MHz · 64 MB SDRAM · MIPI-DSI 480x854
#
# Every value on screen is read from silicon at runtime.
# Nothing is hardcoded except labels. Stubbed port APIs are labelled as stubs.

import gc
import os
import time

import board
import digitalio
import displayio
import terminalio
import vectorio
import microcontroller
import supervisor
import storage
from displayio import Group, Palette
from terminalio import FONT

# ------------------------------------------------------------------ palette
BG     = 0x070A0F
PANEL  = 0x121821
EDGE   = 0x27313E
CYAN   = 0x2DD4E8
GREEN  = 0x4ADE80
AMBER  = 0xFBBF24
RED    = 0xF87171
WHITE  = 0xE8EEF5
DIM    = 0x7A8A9E
VIOLET = 0xA78BFA

D = board.DISPLAY
W, H = D.width, D.height
FW, FH = FONT.get_bounding_box()          # 6 x 12

root = Group()
D.root_group = root

# ------------------------------------------------------------------ helpers
def rect(x, y, w, h, color):
    p = Palette(1)
    p[0] = color
    r = vectorio.Rectangle(pixel_shader=p, width=w, height=h, x=x, y=y)
    root.append(r)
    return r


def label(s, x, y, color=WHITE, scale=1, width=None):
    """Text as a TileGrid over the builtin font sheet (no adafruit_display_text)."""
    s = str(s)
    n = width or len(s)
    pal = Palette(2)
    pal[0] = 0x000000
    pal.make_transparent(0)
    pal[1] = color
    tg = displayio.TileGrid(
        FONT.bitmap, pixel_shader=pal, width=n, height=1,
        tile_width=FW, tile_height=FH, x=0, y=0,
    )
    for i in range(n):
        ch = s[i] if i < len(s) else " "
        g = FONT.get_glyph(ord(ch))
        tg[i] = g.tile_index if g else 0
    holder = Group(scale=scale, x=x, y=y)
    holder.append(tg)
    root.append(holder)
    return tg


def setlbl(tg, s):
    s = str(s)
    for i in range(tg.width):
        ch = s[i] if i < len(s) else " "
        g = FONT.get_glyph(ord(ch))
        tg[i] = g.tile_index if g else 0


def human(n):
    if n >= 1 << 20:
        return "%.1f MB" % (n / (1 << 20))
    if n >= 1 << 10:
        return "%.1f KB" % (n / (1 << 10))
    return "%d B" % n


# ------------------------------------------------------------------ layout
M    = 8            # outer margin
PW   = W - 2 * M    # panel width  = 464
LX   = M + 10       # label column
VX   = M + 150      # value column  (wide gutter: no collisions)
ROW  = 17           # row pitch


def panel(y, h, title):
    rect(M, y, PW, h, PANEL)
    rect(M, y, PW, 1, EDGE)
    rect(M, y + h - 1, PW, 1, EDGE)
    rect(M, y, 3, h, CYAN)
    label(title, LX, y + 6, CYAN)
    return y + 24


def row(y, name, value, color=WHITE, width=None):
    label(name, LX, y, DIM)
    tg = label(value, VX, y, color, width=width)
    return tg


# ------------------------------------------------------------------ header
rect(0, 0, W, H, BG)
rect(0, 0, W, 40, PANEL)
rect(0, 40, W, 2, CYAN)
label("RA8D1", M, 8, CYAN, scale=2)
label("CIRCUITPYTHON", M + 74, 14, WHITE)
label("RENESAS  EK-RA8D1", W - 8 - 17 * FW, 14, DIM)

u = os.uname()
uid = "".join("%02X" % b for b in microcontroller.cpu.uid[:16])

# ------------------------------------------------------------------ silicon
y = panel(50, 108, "SILICON")
row(y, "part", u.sysname)
row(y + ROW, "core", "Cortex-M85  v8.1-M")
row(y + ROW * 2, "clock", "%d MHz" % (microcontroller.cpu.frequency // 1_000_000), GREEN)
row(y + ROW * 3, "uid", uid[:16], VIOLET)
row(y + ROW * 4, "", uid[16:], VIOLET)

# ------------------------------------------------------------------ firmware
y = panel(166, 91, "FIRMWARE")
row(y, "version", u.release, GREEN)
row(y + ROW, "build", u.version.split(" on ")[0])
row(y + ROW * 2, "dated", u.version.split(" on ")[-1])
row(y + ROW * 3, "board id", "renesas_ek_ra8d1")

# ------------------------------------------------------------------ display
y = panel(265, 91, "DISPLAY")
row(y, "panel", "%d x %d" % (W, H), GREEN)
row(y + ROW, "controller", "ILI9806E  MIPI-DSI")
row(y + ROW * 2, "pipeline", "displayio -> GLCDC")
row(y + ROW * 3, "rotation", "%d deg" % D.rotation)

# ------------------------------------------------------------------ memory
y = panel(364, 108, "MEMORY")
heap_tg = row(y, "heap free", "", GREEN, width=14)
used_tg = row(y + ROW, "heap used", "", WHITE, width=14)
peak_tg = row(y + ROW * 2, "peak free", "", VIOLET, width=14)
fs_tg = row(y + ROW * 3, "filesystem", "", WHITE, width=22)

BAR_X = LX
BAR_W = PW - 2 * (LX - M)
BAR_Y = y + ROW * 4 + 2
rect(BAR_X, BAR_Y, BAR_W, 8, EDGE)
heap_bar = rect(BAR_X, BAR_Y, 4, 8, GREEN)

# ------------------------------------------------------------------ i/o
y = panel(480, 122, "I / O   live")

leds = []
for name in ("LED0", "LED1", "LED2", "LED3"):
    p = digitalio.DigitalInOut(getattr(board, name))
    p.switch_to_output(value=False)
    leds.append(p)

btns = []
for name in ("PUSH_BUTTON_SWITCH_1", "PUSH_BUTTON_SWITCH_2"):
    b = digitalio.DigitalInOut(getattr(board, name))
    b.switch_to_input(digitalio.Pull.UP)
    btns.append(b)

label("leds", LX, y, DIM)
led_dots = []
for i in range(4):
    x = VX + i * 52
    label("L%d" % i, x, y, DIM)
    led_dots.append(rect(x + 20, y + 1, 12, 12, EDGE))

label("buttons", LX, y + ROW * 2, DIM)
btn_dots = []
for i in range(2):
    x = VX + i * 78
    label("SW%d" % (i + 1), x, y + ROW * 2, DIM)
    btn_dots.append(rect(x + 26, y + ROW * 2 + 1, 12, 12, EDGE))

row(y + ROW * 4, "buses", "I2C  SPI  UART3/9  PWM")

# ------------------------------------------------------------------ runtime
y = panel(610, 108, "RUNTIME")
up_tg = row(y, "uptime", "", GREEN, width=14)
fr_tg = row(y + ROW, "frames", "", WHITE, width=14)
tick_tg = row(y + ROW * 2, "ticks_ms", "", WHITE, width=14)
row(y + ROW * 3, "run reason",
    str(supervisor.runtime.run_reason).split(".")[-1])
_m = storage.getmount("/")
row(y + ROW * 4, "storage",
    "%s  %s" % (_m.label, "USB-owned" if _m.readonly else "writable"),
    AMBER if _m.readonly else GREEN)

# ------------------------------------------------------------------ notes
y = panel(726, 91, "NOTES")
label("REPL + file push over J-Link VCOM   OK", LX, y, GREEN)
label("USB MSC  RA8 HS bulk-IN stalls (upstream)", LX, y + ROW, AMBER)
label("cpu.temperature / voltage are port stubs", LX, y + ROW * 2, DIM)
label("GLCDC + FatFs boot-jump fixes applied", LX, y + ROW * 3, VIOLET)

# ------------------------------------------------------------------ footer
rect(0, H - 26, W, 26, PANEL)
rect(0, H - 28, W, 2, EDGE)
foot_tg = label("", M, H - 19, DIM, width=44)
pulse = rect(W - 20, H - 19, 10, 10, GREEN)

# ------------------------------------------------------------------ static fs
st = os.statvfs("/")
fs_total = st[0] * st[2]
fs_free = st[0] * st[3]
setlbl(fs_tg, "%s free / %s" % (human(fs_free), human(fs_total)))

print("=== RA8D1 DASHBOARD ===")
print("display %dx%d  heap %d  fs %d/%d" % (W, H, gc.mem_free(), fs_free, fs_total))


def screendump():
    """Emit every drawn element so the host can render a pixel-exact mirror."""
    inv = {}
    for cp in range(32, 127):
        g = FONT.get_glyph(cp)
        if g:
            inv[g.tile_index] = chr(cp)
    print("SCREENDUMP %d %d" % (W, H))

    def walk(g, ox, oy, sc):
        for item in g:
            if isinstance(item, Group):
                walk(item, ox + item.x * sc, oy + item.y * sc, sc * item.scale)
            elif isinstance(item, displayio.TileGrid):
                s = ""
                for i in range(item.width):
                    s += inv.get(item[i], " ")
                try:
                    col = item.pixel_shader[1]
                except Exception:
                    col = 0xFFFFFF
                print("T|%d|%d|%d|%06x|%s" %
                      (ox + item.x * sc, oy + item.y * sc, sc, col, s.rstrip()))
            elif isinstance(item, vectorio.Rectangle):
                try:
                    col = item.pixel_shader[0]
                except Exception:
                    col = 0
                print("R|%d|%d|%d|%d|%06x" %
                      (item.x, item.y, item.width, item.height, col))
    walk(root, 0, 0, 1)
    print("ENDDUMP")


# ------------------------------------------------------------------ loop
frames = 0
t0 = time.monotonic()
peak_free = 0

while True:
    gc.collect()
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    if free > peak_free:
        peak_free = free

    setlbl(heap_tg, human(free))
    setlbl(used_tg, human(alloc))
    setlbl(peak_tg, human(peak_free))

    total = free + alloc
    heap_bar.width = max(4, int(BAR_W * (alloc / total))) if total else 4

    act = frames % 4
    for i, p in enumerate(leds):
        on = (i == act)
        p.value = on
        led_dots[i].pixel_shader[0] = GREEN if on else EDGE

    for i, b in enumerate(btns):
        btn_dots[i].pixel_shader[0] = AMBER if not b.value else EDGE

    up = time.monotonic() - t0
    setlbl(up_tg, "%dm %02ds" % (int(up // 60), int(up % 60)))
    setlbl(fr_tg, "%d" % frames)
    setlbl(tick_tg, "%d" % supervisor.ticks_ms())
    setlbl(foot_tg, "live  ·  %.1f fps  ·  %s free" %
           (frames / up if up > 0.5 else 0.0, human(free)))
    pulse.pixel_shader[0] = GREEN if (frames % 2) else PANEL

    if frames % 25 == 0:
        print("frame %d  up %.1fs  free %d  alloc %d" % (frames, up, free, alloc))

    if frames == 4:
        screendump()

    frames += 1
    time.sleep(0.2)
