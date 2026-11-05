#!/usr/bin/env python3
"""Compares a native picture pixel by pixel with a screenshot of the melonDS window.

The window has a border and a menu, so the tool searches where the 256 x 384 picture sits. Counts
pixels that differ at all and pixels that differ by more than a rounding step, and writes an image
that shows the differences in red on a darkened copy.

Usage: python tools/compare_picture.py <frame.rgba> <melonds.png> [diff.png]
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import write_png  # noqa: E402

WIDTH, HEIGHT = 256, 384
ROUNDING = 8  # one 5-bit colour step is 8 in 8-bit colour


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    channels = {2: 3, 6: 4}[color]
    if depth != 8:
        raise ValueError("needs 8 bits per channel")
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        kind = raw[pos]
        row = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = prev[i]
            corner = prev[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + left) & 255
            elif kind == 2:
                row[i] = (row[i] + up) & 255
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                pred = left if pa <= pb and pa <= pc else up if pb <= pc else corner
                row[i] = (row[i] + pred) & 255
        for x in range(width):
            out[(y * width + x) * 4:(y * width + x) * 4 + 3] = row[x * channels:x * channels + 3]
            out[(y * width + x) * 4 + 3] = 255
        prev = row
    return width, height, out


def differences(native, shot, width, dx, dy, step=1):
    count = 0
    for y in range(0, HEIGHT, step):
        for x in range(0, WIDTH, step):
            a = (y * WIDTH + x) * 4
            b = ((y + dy) * width + x + dx) * 4
            if native[a:a + 3] != shot[b:b + 3]:
                count += 1
    return count


def main():
    with open(sys.argv[1], "rb") as f:
        native = f.read()
    width, height, shot = read_png(sys.argv[2])
    best = None
    for dy in range(0, height - HEIGHT + 1):
        for dx in range(0, width - WIDTH + 1):
            score = differences(native, shot, width, dx, dy, step=8)
            if best is None or score < best[0]:
                best = (score, dx, dy)
    _, dx, dy = best
    exact = near = 0
    diff = bytearray(len(native))
    worst = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            a = (y * WIDTH + x) * 4
            b = ((y + dy) * width + x + dx) * 4
            delta = max(abs(native[a + i] - shot[b + i]) for i in range(3))
            worst = max(worst, delta)
            if delta == 0:
                exact += 1
            if delta < ROUNDING:
                near += 1
            hit = delta >= ROUNDING
            diff[a:a + 4] = bytes((255, 0, 0, 255)) if hit else bytes(
                (native[a] // 3, native[a + 1] // 3, native[a + 2] // 3, 255))
    total = WIDTH * HEIGHT
    print(f"position in window: x={dx} y={dy}")
    print(f"exactly equal: {exact}/{total} ({100 * exact / total:.3f}%)")
    print(f"within one colour step: {near}/{total} ({100 * near / total:.3f}%), largest difference {worst}")
    if len(sys.argv) > 3:
        write_png(sys.argv[3], WIDTH, HEIGHT, bytes(diff))
    return 0


if __name__ == "__main__":
    sys.exit(main())
