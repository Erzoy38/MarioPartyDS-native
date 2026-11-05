#!/usr/bin/env python3
"""Build a contact sheet from PNGs written by gfx.write_png (no filtering, RGBA).

Usage: python tools/contact_sheet.py <out.png> <columns> <file> [<file> ...]
       python tools/contact_sheet.py <out.png> <columns> --sample <count> [--seed N]
       python tools/contact_sheet.py <out.png> <columns> --list <textfile>

Images are reduced by nearest neighbour to fit a 128x96 cell, over a grey background.
"""
import glob
import os
import random
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import write_png  # noqa: E402

CELL_W, CELL_H = 128, 96
GAP = 2
BACK = (70, 70, 80)


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    pos = 8
    width = height = 0
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * 4 + 1
    rows = bytearray()
    for y in range(height):
        rows += raw[y * stride + 1:(y + 1) * stride]
    return width, height, bytes(rows)


def main():
    out, columns = sys.argv[1], int(sys.argv[2])
    args = sys.argv[3:]
    if args[0] == "--sample":
        count = int(args[1])
        seed = int(args[3]) if len(args) > 3 else 1
        files = sorted(glob.glob("gfx/screen_*.png"))
        random.Random(seed).shuffle(files)
        files = sorted(files[:count])
    elif args[0] == "--list":
        with open(args[1]) as f:
            files = [line.strip() for line in f if line.strip()]
    else:
        files = args

    rows = (len(files) + columns - 1) // columns
    width = columns * (CELL_W + GAP) + GAP
    height = rows * (CELL_H + GAP) + GAP
    canvas = bytearray(bytes((20, 20, 24, 255)) * (width * height))
    for i, path in enumerate(files):
        src_w, src_h, rgba = read_png(path)
        scale = max(src_w / CELL_W, src_h / CELL_H, 1)
        dst_w, dst_h = int(src_w / scale), int(src_h / scale)
        ox = (i % columns) * (CELL_W + GAP) + GAP
        oy = (i // columns) * (CELL_H + GAP) + GAP
        for y in range(CELL_H):
            for x in range(CELL_W):
                pos = (oy + y) * width * 4 + (ox + x) * 4
                pixel = BACK
                if x < dst_w and y < dst_h:
                    sx, sy = int(x * scale), int(y * scale)
                    src = (sy * src_w + sx) * 4
                    if rgba[src + 3]:
                        pixel = tuple(rgba[src:src + 3])
                canvas[pos:pos + 4] = bytes((*pixel, 255))
    write_png(out, width, height, bytes(canvas))
    with open(out + ".txt", "w") as f:
        for i, path in enumerate(files):
            f.write(f"row {i // columns} col {i % columns}: {os.path.basename(path)}\n")
    print(f"{out}: {width}x{height}, {len(files)} images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
