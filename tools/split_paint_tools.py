#!/usr/bin/env python3
"""split_paint_tools - slice assets/paint-tools.png (a 2-col x 7-row grid of white-on-
black tool icons in bordered boxes) into the 14 individual PAINT toolchest icons
(assets/paint/<name>.png, 24x24), auto-trimming + centring each glyph. Re-run this then
png2cpc each PNG -> .asm when the source sheet changes (#246).

  tools/split_paint_tools.py
"""
import os
from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "assets", "paint-tools.png")
im = Image.open(src).convert("L")

# box interiors (detected from the sheet's light separators): ~42px row pitch, 2 cols.
YBANDS = [(16, 52), (58, 94), (100, 136), (142, 179), (184, 221), (226, 263), (268, 305)]
XBANDS = [(7, 47), (54, 92)]
# read order (row-major) -> final tool names (#246: idx0 is the spray can, idx7 the
# squiggle is the pencil/freehand stroke; idx5 is a separate can = bucket)
NAMES = ["spray", "text", "fill", "eraser", "circle", "bucket", "line", "pencil",
         "boxfill", "square", "undo", "arc", "picker", "select"]

i = 0
for (y0, y1) in YBANDS:
    for (x0, x1) in XBANDS:
        cell = im.crop((x0, y0, x1, y1)).point(lambda v: 255 if v > 120 else 0)
        bb = cell.getbbox()                       # bbox of the white glyph
        g = cell.crop(bb) if bb else cell
        gw, gh = g.size
        s = min(22 / gw, 22 / gh)                 # fit within 22x22, leaving a margin
        g = g.resize((max(1, round(gw * s)), max(1, round(gh * s))), Image.LANCZOS) \
             .point(lambda v: 255 if v > 110 else 0)
        canvas = Image.new("L", (24, 24), 0)      # pen-2 black background
        canvas.paste(g, ((24 - g.width) // 2, (24 - g.height) // 2))
        canvas.convert("RGB").save(os.path.join(root, "assets", "paint", NAMES[i] + ".png"))
        i += 1
print("wrote %d icons to assets/paint/" % i)
