#!/usr/bin/env python3
"""Test the animation hypothesis on 'offsets' leaves that are not frame sets.

Hypothesis: u32 N, u32 offsets[N] from +4. Sequence: 4-byte steps (u8 duration, u8 frame,
u16 extra), ending with a step of duration 0. If the leaf right after a frame set is its
animation, every frame number must be below that set's frame count.

Usage: python tools/check_anims.py
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from render_screen import source_files  # noqa: E402
from render_sprites import parse_frames  # noqa: E402


def parse_anim(data):
    """Return sequences of (duration, frame, extra), or None if the layout does not fit."""
    count = struct.unpack_from("<I", data, 0)[0]
    offsets = struct.unpack_from(f"<{count}I", data, 4)
    if list(offsets) != sorted(offsets):
        return None
    sequences = []
    for i, off in enumerate(offsets):
        start = off + 4
        end = offsets[i + 1] + 4 if i + 1 < count else len(data)
        if (end - start) % 4 or end > len(data) or end <= start:
            return None
        steps = [struct.unpack_from("<BBH", data, p) for p in range(start, end, 4)]
        if steps[-1][0] != 0 or any(s[0] == 0 for s in steps[:-1]):
            return None
        sequences.append(steps)
    return sequences


def main():
    tally = collections.Counter()
    extra = collections.Counter()
    terminator = collections.Counter()
    samples = []
    for name, leaves in source_files():
        groups = collections.defaultdict(list)
        for leaf, data, _depth in leaves:
            groups[leaf.rsplit("#", 1)[0]].append((leaf, data, classify(data)))
        for parent, items in groups.items():
            for pos, (leaf, data, kind) in enumerate(items):
                if kind != "offsets" or parse_frames(data) is not None:
                    continue
                tally["leaves"] += 1
                sequences = parse_anim(data)
                if sequences is None:
                    tally["layout does not fit"] += 1
                    if len(samples) < 8:
                        samples.append((leaf, len(data), data[:32].hex(" ")))
                    continue
                tally["layout fits"] += 1
                for steps in sequences:
                    terminator[steps[-1][1:]] += 1
                    for step in steps[:-1]:
                        extra[step[2]] += 1
                top = max((s[1] for seq in sequences for s in seq[:-1]), default=0)
                before = items[pos - 1] if pos else None
                if before and before[2] == "offsets" and parse_frames(before[1]) is not None:
                    frame_count = len(parse_frames(before[1]))
                    tally["follows a frame set"] += 1
                    tally["  frame numbers fit" if top < frame_count else "  frame numbers too high"] += 1
                else:
                    tally["does not follow a frame set"] += 1
    for key, value in tally.most_common():
        print(f"{key:32}{value}")
    print(f"step extra field (top 8): {extra.most_common(8)}")
    print(f"terminator (frame, extra) (top 8): {terminator.most_common(8)}")
    for item in samples:
        print(" ", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
