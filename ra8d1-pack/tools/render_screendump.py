#!/usr/bin/env python3
"""Render a pixel-exact mirror of the RA8D1 dashboard from a SCREENDUMP.

Reads T| (text) and R| (rect) records and draws them with the real 6x12
CircuitPython terminalio font metrics, so the output matches the panel.
"""
import re
import sys
from PIL import Image, ImageDraw, ImageFont

FW, FH = 6, 12
SCALE = 2          # output upscale

recs = []
W = H = None
for path in sys.argv[1:]:
    for line in open(path, errors="replace"):
        line = line.rstrip("\r\n")
        m = re.match(r"^SCREENDUMP (\d+) (\d+)", line)
        if m:
            W, H = int(m.group(1)), int(m.group(2))
            continue
        if line.startswith("R|"):
            p = line.split("|")
            if len(p) >= 6:
                recs.append(("R", int(p[1]), int(p[2]), int(p[3]), int(p[4]), p[5][:6]))
        elif line.startswith("T|"):
            p = line.split("|", 5)
            if len(p) >= 6:
                recs.append(("T", int(p[1]), int(p[2]), int(p[3]), p[4][:6], p[5]))

if W is None:
    W, H = 480, 854
    print("no SCREENDUMP header, assuming %dx%d" % (W, H))

# de-dup: keep the LAST record for a given (kind, x, y) so live values win
seen = {}
for r in recs:
    key = (r[0], r[1], r[2]) if r[0] == "T" else ("R", r[1], r[2], r[3], r[4])
    seen[key] = r
recs = list(seen.values())

rects = [r for r in recs if r[0] == "R"]
texts = [r for r in recs if r[0] == "T"]
print("rects %d  texts %d  canvas %dx%d" % (len(rects), len(texts), W, H))

img = Image.new("RGB", (W * SCALE, H * SCALE), (7, 10, 15))
d = ImageDraw.Draw(img)

def rgb(h):
    v = int(h, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)

# rects first (background), in original order so later ones overlay
for _, x, y, w, h, c in rects:
    d.rectangle([x * SCALE, y * SCALE, (x + w) * SCALE - 1, (y + h) * SCALE - 1],
                fill=rgb(c))

font_paths = [
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
]
def load(sz):
    for p in font_paths:
        try:
            return ImageFont.truetype(p, sz)
        except Exception:
            pass
    return ImageFont.load_default()

for _, x, y, sc, c, s in texts:
    if not s.strip():
        continue
    size = int(FH * sc * SCALE * 0.82)
    f = load(size)
    # advance-match the real font: draw char by char on the 6px grid
    col = rgb(c)
    for i, ch in enumerate(s):
        if ch == " ":
            continue
        d.text(((x + i * FW * sc) * SCALE, y * SCALE), ch, font=f, fill=col)

out = "/tmp.png"
img.save(out)
print("saved", out, img.size)
