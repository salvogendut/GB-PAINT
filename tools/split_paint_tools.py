#!/usr/bin/env python3
"""Build PAINT's 24x21 toolchest icons from assets/paint-tools.png.

The source sheet contains fourteen original glyphs. Circle-fill, cut, copy and
paste are drawn in the same compact monochrome style because they are not
present in that sheet. Re-run this and then png2cpc when the source changes.

  tools/split_paint_tools.py
"""
import os
from PIL import Image, ImageDraw

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


def save_glyph(glyph, name):
    """Scale and centre a monochrome glyph in one 24x21 tool cell."""
    bb = glyph.getbbox()
    g = glyph.crop(bb) if bb else glyph
    gw, gh = g.size
    scale = min(22 / gw, 19 / gh)
    g = g.resize(
        (max(1, round(gw * scale)), max(1, round(gh * scale))),
        Image.Resampling.LANCZOS,
    ).point(lambda value: 255 if value > 110 else 0)
    canvas = Image.new("L", (24, 21), 0)
    canvas.paste(g, ((24 - g.width) // 2, (21 - g.height) // 2))
    canvas.convert("RGB").save(
        os.path.join(root, "assets", "paint", name + ".png")
    )


for (y0, y1) in YBANDS:
    for (x0, x1) in XBANDS:
        cell = im.crop((x0, y0, x1, y1)).point(lambda v: 255 if v > 120 else 0)
        save_glyph(cell, NAMES[i])
        i += 1


def generated_icon(name, draw_icon):
    canvas = Image.new("L", (24, 21), 0)
    draw_icon(ImageDraw.Draw(canvas))
    canvas.convert("RGB").save(
        os.path.join(root, "assets", "paint", name + ".png")
    )


generated_icon(
    "circlefill",
    lambda draw: draw.ellipse((4, 2, 19, 18), fill=255),
)


def draw_cut(draw):
    draw.ellipse((2, 2, 8, 8), outline=255, width=2)
    draw.ellipse((2, 12, 8, 18), outline=255, width=2)
    draw.line((7, 7, 20, 17), fill=255, width=2)
    draw.line((7, 13, 20, 3), fill=255, width=2)


def draw_copy(draw):
    draw.rectangle((3, 2, 15, 14), outline=255, width=2)
    draw.rectangle((8, 7, 21, 19), outline=255, width=2)


def draw_paste(draw):
    draw.rectangle((4, 4, 20, 19), outline=255, width=2)
    draw.rounded_rectangle((8, 1, 16, 7), radius=2, outline=255, width=2)
    draw.line((8, 10, 16, 10), fill=255, width=2)
    draw.line((8, 14, 16, 14), fill=255, width=2)


generated_icon("cut", draw_cut)
generated_icon("copy", draw_copy)
generated_icon("paste", draw_paste)

print("wrote %d atlas icons and 4 generated icons to assets/paint/" % i)
