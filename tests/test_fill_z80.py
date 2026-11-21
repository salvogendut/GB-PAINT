"""Exercise the actual SDCC-linked bucket routine (requires ucsim_z80).

Usage: python3 tests/test_fill_z80.py build/pcw
The directory must contain app.ihx, app.noi and main.lst from the same build.
"""
import re
import subprocess
import sys
from pathlib import Path


def reference(pixels, x, y, pen, w, h):
    result = pixels.copy()
    old = result[y * 20 + x]
    todo = [(x, y)]
    while todo and old != pen:
        x, y = todo.pop()
        if 0 <= x < w and 0 <= y < h and result[y * 20 + x] == old:
            result[y * 20 + x] = pen
            todo.extend(((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)))
    return result


def main(build):
    symbols = {k: int(v, 16) for k, v in re.findall(
        r"^DEF (\S+) 0x([\da-fA-F]+)", (build / "app.noi").read_text(), re.M)}
    offsets = {k: int(v, 16) for v, k in re.findall(
        r"^\s*([\da-fA-F]{6,8})\s+\d+ (_\w+)::?$",
        (build / "main.lst").read_text(), re.M)}
    data = symbols["s__DATA"]
    fill = symbols["_main"] - offsets["_main"] + offsets["_flood_fill"]
    tile = data + offsets["_tile"]
    circle = bytearray([1] * 400)
    for y, row in enumerate((".....#####", "....#....#", "....#....#",
                             "...#.....#", "...#.....#", "...##....#",
                             "....######"), 4):
        for x, ink in enumerate(row):
            if ink == "#":
                circle[y * 20 + x] = 2
    chess = bytearray((x + y) % 2 for y in range(20) for x in range(20))
    for title, pixels, x, y, pen, w, h in (
        ("inside reported outline", circle, 7, 8, 3, 20, 20),
        ("outside reported outline", circle, 19, 19, 15, 20, 20),
        ("entire tile, last pixel seed", bytearray(400), 19, 19, 3, 20, 20),
        ("diagonal boundary", chess, 9, 9, 3, 20, 20),
        ("partial width and height", bytearray(400), 4, 3, 15, 5, 4),
        ("no change", circle, 7, 8, 1, 20, 20),
    ):
        ops = []

        def write(address, values):
            ops.append(f"set memory rom {address} " + " ".join(map(str, values)))

        for key, val in (("_pic_width", [w, 0]), ("_pic_height", [h, 0]),
                         ("_tile_x", [0, 0]), ("_tile_y", [0, 0]),
                         ("_current_pen", [pen]), ("_tile_w", [w]), ("_tile_h", [h])):
            if key in offsets:
                write(data + offsets[key], val)
        for i in range(0, 400, 20):
            write(tile + i, pixels[i:i + 20])
        # LD SP,FF00; LD A,x; LD L,y; CALL flood_fill; stop at return address.
        write(0x1000, [0x31, 0, 255, 0x3e, x, 0x2e, y, 0xcd, fill & 255, fill >> 8])
        ops.extend(("run 0x1000 0x100a", f"dump /h rom {tile} {tile + 399} 16", "quit"))
        out = subprocess.run(["ucsim_z80", "-b", "-q", "-c", "-", str(build / "app.ihx")],
                             input="\n".join(ops) + "\n", capture_output=True, text=True,
                             timeout=10, check=True).stdout
        rows = re.findall(r"^0x[0-9a-f]+ ((?:[0-9a-f]{2} ){16})", out, re.M)
        actual = bytes.fromhex("".join(rows[-25:]))
        assert actual == reference(pixels, x, y, pen, w, h), title
        print("ok:", title)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
