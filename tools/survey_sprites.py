#!/usr/bin/env python3
"""Test the sprite frame hypothesis on every 'offsets' leaf.

Hypothesis: u32 N, u32 offsets[N] (from +4). Frame: u16 parts, then 10 bytes per part:
attr0, attr1, attr2, s16 x, s16 y. The stored x and y repeat the OAM coordinates, which
gives an independent check. Also finds the tile index unit: per frame set, the last
byte used should equal the size of one tile set in the same parent.

Usage: python tools/survey_sprites.py
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from render_screen import source_files  # noqa: E402

SIZES = {  # (shape, size) -> (width, height)
    (0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
    (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
    (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64),
}


def parse_frames(data):
    """Return a list of frames, each a list of part tuples, or None."""
    count = struct.unpack_from("<I", data, 0)[0]
    offsets = struct.unpack_from(f"<{count}I", data, 4)
    if list(offsets) != sorted(offsets):
        return None
    frames = []
    for i, off in enumerate(offsets):
        start = off + 4
        end = offsets[i + 1] + 4 if i + 1 < count else len(data)
        if start + 2 > len(data):
            return None
        parts = struct.unpack_from("<H", data, start)[0]
        if end - start != 2 + parts * 10:
            return None
        frame = []
        for p in range(parts):
            frame.append(struct.unpack_from("<HHHhh", data, start + 2 + p * 10))
        frames.append(frame)
    return frames


def sext(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def main():
    tally = collections.Counter()
    coords = collections.Counter()
    attr = collections.Counter()
    units = collections.Counter()
    samples = []
    anim_samples = []
    for name, leaves in source_files():
        groups = collections.defaultdict(list)
        for leaf, data, _depth in leaves:
            groups[leaf.rsplit("#", 1)[0]].append((leaf, data, classify(data)))
        for parent, items in groups.items():
            tile_sizes = collections.defaultdict(set)
            for leaf, data, kind in items:
                if kind.startswith("tiles"):
                    bpp = 8 if kind.startswith("tiles8") else 4
                    tile_sizes[bpp].add(len(data) - 4)
            for leaf, data, kind in items:
                if kind != "offsets":
                    continue
                tally["offsets leaves"] += 1
                frames = parse_frames(data)
                if frames is None:
                    tally["not frames"] += 1
                    if len(anim_samples) < 8:
                        anim_samples.append((leaf, len(data), data[:40].hex(" ")))
                    continue
                tally["parse exactly as frames"] += 1
                if sum(len(f) for f in frames) == 0:
                    tally["  frames, all empty"] += 1
                    continue
                last = {32: 0, 64: 0, 128: 0, 256: 0}
                bpp_seen = set()
                for frame in frames:
                    for attr0, attr1, attr2, x, y in frame:
                        coords["x matches attr1" if sext(attr1 & 0x1FF, 9) == x else "x differs"] += 1
                        coords["y matches attr0" if sext(attr0 & 0xFF, 8) == y else "y differs"] += 1
                        shape, size = attr0 >> 14, attr1 >> 14
                        attr[f"shape{shape}"] += 1
                        attr["8bpp" if attr0 >> 13 & 1 else "4bpp"] += 1
                        if attr0 >> 8 & 3:
                            attr["rotation or hidden bits"] += 1
                        if attr0 >> 10 & 3:
                            attr["obj mode bits"] += 1
                        if attr1 >> 12 & 3:
                            attr["flip bits"] += 1
                        if shape == 3:
                            attr["invalid shape 3"] += 1
                            continue
                        width, height = SIZES[(shape, size)]
                        bpp = 8 if attr0 >> 13 & 1 else 4
                        bpp_seen.add(bpp)
                        bytes_used = width * height * bpp // 8
                        for unit in last:
                            last[unit] = max(last[unit], (attr2 & 0x3FF) * unit + bytes_used)
                if len(bpp_seen) == 1:
                    bpp = bpp_seen.pop()
                    matched = [u for u, v in last.items() if v in tile_sizes[bpp]]
                    units[(bpp, tuple(matched))] += 1
                    if not matched and len(samples) < 10:
                        samples.append((leaf, bpp, {u: hex(v) for u, v in last.items()},
                                        sorted(hex(s) for s in tile_sizes[bpp])[:6]))
                else:
                    tally["  mixed or no bpp"] += 1

    for key, value in tally.most_common():
        print(f"{key:30}{value}")
    print("\ncoordinates:", dict(coords))
    print("attributes:", dict(attr))
    print("\ntile index unit by exact fit to a tile set (bpp, matching units): count")
    for key, value in units.most_common(16):
        print(f"  {key}: {value}")
    print("\nframe sets with no exact fit (first 10):")
    for item in samples:
        print(" ", item)
    print("\nnot-frame samples:")
    for item in anim_samples:
        print(" ", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
