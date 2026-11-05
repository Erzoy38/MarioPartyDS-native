#!/usr/bin/env python3
"""Finds which native picture in a range is closest to a screenshot of the melonDS window.

The two runs are offset in time, so moving screens match at some other tick, if at all.
Usage: python tools/find_matching_frame.py <frames folder> <from> <to> <melonds.png> [diff.png]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_picture import HEIGHT, WIDTH, read_png  # noqa: E402
from gfx import write_png  # noqa: E402

LEFT, TOP = 8, 53  # where the picture sits in the window, found by compare_picture.py


def main():
    folder, first, last, shot_path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    width, _height, shot = read_png(shot_path)
    rows = [bytes(shot[((y + TOP) * width + LEFT) * 4:((y + TOP) * width + LEFT + WIDTH) * 4])
            for y in range(HEIGHT)]
    best = None
    for tick in range(first, last + 1):
        path = os.path.join(folder, f"{tick}.rgba")
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            frame = f.read()
        bad = 0
        for y in range(HEIGHT):
            row = frame[y * WIDTH * 4:(y + 1) * WIDTH * 4]
            if row != rows[y]:
                bad += sum(row[i:i + 3] != rows[y][i:i + 3] for i in range(0, WIDTH * 4, 4))
        if best is None or bad < best[0]:
            best = (bad, tick, frame)
    bad, tick, frame = best
    total = WIDTH * HEIGHT
    top_bad = sum(frame[(y * WIDTH + x) * 4:(y * WIDTH + x) * 4 + 3]
                  != rows[y][x * 4:x * 4 + 3] for y in range(192) for x in range(WIDTH))
    print(f"best tick {tick}: {total - bad}/{total} pixels exactly equal "
          f"({100 * (total - bad) / total:.3f}%); differing: top {top_bad}, bottom {bad - top_bad}")
    if len(sys.argv) > 5:
        diff = bytearray(len(frame))
        for y in range(HEIGHT):
            for x in range(WIDTH):
                a = (y * WIDTH + x) * 4
                same = frame[a:a + 3] == rows[y][x * 4:x * 4 + 3]
                diff[a:a + 4] = bytes((frame[a] // 3, frame[a + 1] // 3, frame[a + 2] // 3, 255)) if same \
                    else bytes((255, 0, 0, 255))
        write_png(sys.argv[5], WIDTH, HEIGHT, bytes(diff))
    return 0


if __name__ == "__main__":
    sys.exit(main())
