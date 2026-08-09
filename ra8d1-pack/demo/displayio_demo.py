# displayio stress/demo for the Renesas EK-RA8D1 (480x854 MIPI-DSI, ILI9806E)
#
# Exercises the parts of displayio that matter on this board:
#   - a real Bitmap + Palette blitted through a scaled Group
#   - animation by PALETTE ROTATION (no per-frame pixel writes, so the cost
#     is the panel push, not Python)
#   - vectorio shapes composited over the bitmap
#   - live text from the builtin font (no adafruit_display_text on this port)
#
# Text technique is borrowed from the pack's dashboard: a TileGrid over
# FONT.bitmap, since adafruit_display_text is not available here.

import gc
import math
import time

import board
import displayio
import terminalio
import vectorio
from displayio import Group, Palette
from terminalio import FONT

D = board.DISPLAY
W, H = D.width, D.height
FW, FH = FONT.get_bounding_box()

root = Group()
D.root_group = root

# ---------------------------------------------------------------- background
# Small bitmap scaled up: 6,420 pixels to compute instead of 410,000.
# At scale 8 this covers 480x856, just past the 854-pixel panel.
SCALE = 8
BW, BH = W // SCALE, H // SCALE + 1
NCOLORS = 32

bmp = displayio.Bitmap(BW, BH, NCOLORS)
pal = Palette(NCOLORS)

# Interference pattern: two radial sources + a diagonal wave. Values are
# palette INDICES, so the pixels never change again after this loop.
cx1, cy1 = BW * 0.30, BH * 0.25
cx2, cy2 = BW * 0.75, BH * 0.70
for y in range(BH):
    for x in range(BW):
        d1 = math.sqrt((x - cx1) ** 2 + (y - cy1) ** 2)
        d2 = math.sqrt((x - cx2) ** 2 + (y - cy2) ** 2)
        v = math.sin(d1 * 0.45) + math.sin(d2 * 0.38) + math.sin((x + y) * 0.16)
        bmp[x, y] = int((v + 3.0) * (NCOLORS - 1) / 6.0) % NCOLORS

bg_holder = Group(scale=SCALE)
bg_holder.append(displayio.TileGrid(bmp, pixel_shader=pal, x=0, y=0))
root.append(bg_holder)


def wheel(i, phase):
    """Deep blue -> teal -> magenta sweep, rotated by phase."""
    t = ((i + phase) % NCOLORS) / NCOLORS
    r = int(70 + 120 * math.sin(math.pi * t))
    g = int(40 + 150 * math.sin(math.pi * (t + 0.33)))
    b = int(90 + 140 * math.sin(math.pi * (t + 0.66)))
    return (max(0, min(255, r)) << 16) | (max(0, min(255, g)) << 8) | max(0, min(255, b))


# ------------------------------------------------------------------- shapes
shape_pals = []
shapes = []
for color, rad, x, y, dx, dy in (
    (0x2DD4E8, 34, 90, 200, 5, 4),
    (0xA78BFA, 26, 300, 430, -4, 5),
    (0x4ADE80, 20, 190, 640, 6, -3),
):
    p = Palette(1)
    p[0] = color
    c = vectorio.Circle(pixel_shader=p, radius=rad, x=x, y=y)
    root.append(c)
    shape_pals.append(p)
    shapes.append([c, dx, dy, rad])


# --------------------------------------------------------------------- text
def label(s, x, y, color=0xE8EEF5, scale=1, width=None):
    s = str(s)
    n = width or len(s)
    p = Palette(2)
    p[0] = 0x000000
    p.make_transparent(0)
    p[1] = color
    tg = displayio.TileGrid(
        FONT.bitmap, pixel_shader=p, width=n, height=1,
        tile_width=FW, tile_height=FH, x=0, y=0,
    )
    for i in range(n):
        g = FONT.get_glyph(ord(s[i] if i < len(s) else " "))
        tg[i] = g.tile_index if g else 0
    holder = Group(scale=scale, x=x, y=y)
    holder.append(tg)
    root.append(holder)
    return tg


def setlbl(tg, s):
    s = str(s)
    for i in range(tg.width):
        g = FONT.get_glyph(ord(s[i] if i < len(s) else " "))
        tg[i] = g.tile_index if g else 0


label("displayio", 12, 12, 0x2DD4E8, scale=3)
label("RA8D1  480x854  MIPI-DSI", 12, 52, 0x7A8A9E)
l_frame = label("frame    ....", 12, H - 76, 0xE8EEF5, width=22)
l_fps = label("fps      ....", 12, H - 56, 0x4ADE80, width=22)
l_heap = label("heap     ....", 12, H - 36, 0xFBBF24, width=22)

gc.collect()
free0 = gc.mem_free()
print("displayio demo up: %dx%d, bitmap %dx%d x%d, heap free %d"
      % (W, H, BW, BH, SCALE, free0))
print("auto_refresh=%s" % getattr(D, "auto_refresh", "?"))

# Paint the initial palette and hold the scene STATIC for a few seconds.
# The panel pushes at roughly 3 fps, so an unthrottled animation loop
# re-dirties the framebuffer faster than a refresh can complete and you
# get a blank panel. Prove the static scene renders first.
for i in range(NCOLORS):
    pal[i] = wheel(i, 0)
try:
    D.refresh()
except Exception as e:
    print("refresh:", type(e).__name__, e)
print("static scene painted, holding 4s")
time.sleep(4)
print("starting animation")

# --------------------------------------------------------------------- loop
FRAME_DELAY = 0.30          # ~3 fps, matched to what the panel can actually push
frame = 0
t0 = time.monotonic()
last = t0
while True:
    time.sleep(FRAME_DELAY)
    # Animate purely by rewriting 32 palette entries. The bitmap is untouched.
    for i in range(NCOLORS):
        pal[i] = wheel(i, frame * 2)

    for s in shapes:
        c, dx, dy, rad = s
        nx, ny = c.x + dx, c.y + dy
        if nx < rad or nx > W - rad:
            dx = -dx
            nx = c.x + dx
        if ny < rad or ny > H - rad:
            dy = -dy
            ny = c.y + dy
        c.x, c.y = nx, ny
        s[1], s[2] = dx, dy

    frame += 1
    now = time.monotonic()
    if now - last >= 1.0:
        gc.collect()
        setlbl(l_frame, "frame    %d" % frame)
        setlbl(l_fps, "fps      %.1f" % (frame / (now - t0)))
        setlbl(l_heap, "heap     %.1f MB" % (gc.mem_free() / 1048576))
        last = now
