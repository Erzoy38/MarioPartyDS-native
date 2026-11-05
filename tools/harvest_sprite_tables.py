#!/usr/bin/env python3
"""Harvest sprite descriptors from the code images: the game's own pairing.

Seen in overlay 1 (0x02169B74, table 0x0216E5A4): records of s16 container indices
(tiles, palette, frames, animation, -1, 0). Menus use (palette, tiles, frames, animation).

A candidate counts only if, in a data file the same image names, +0 and +2 are a tile set
and a palette in either order, +4 a frame set that fits that tile set exactly for some
index unit, and +6 an animation (an 'offsets' leaf that is not a frame set) or -1.

Writes sprite_tables.csv. Usage: python tools/harvest_sprite_tables.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from nds_image import all_code_images  # noqa: E402
from render_screen import source_files  # noqa: E402
from render_sprites import UNITS, bytes_needed, frame_bpp, parse_frames  # noqa: E402

SHARED = ("UK/mg_000_data_LZ.bin", "mg_000_data.bin", "mg_000boss_data.bin",
          "UK/mg_pause_data.bin", "UK/mg_demo_data.bin")


def normal_name(name):
    name = name.lstrip("/")
    return name[5:] if name.startswith("data/") else name


def main():
    print("indexing data files...")
    members = collections.defaultdict(dict)
    for rel, leaves in source_files():
        for leaf, data, _depth in leaves:
            parts = leaf.split("#")
            if len(parts) == 2:
                members[rel][int(parts[1])] = (classify(data), data)

    with open("file_refs.csv", newline="") as f:
        names_by_image = collections.defaultdict(set)
        for row in csv.DictReader(f):
            names_by_image[row["image"]].add(normal_name(row["name"]))

    frames_cache = {}

    def frames_of(rel, index):
        key = (rel, index)
        if key not in frames_cache:
            kind, data = members[rel][index]
            frames_cache[key] = parse_frames(data) if kind == "offsets" else None
        return frames_cache[key]

    rows = []
    tally = collections.Counter()
    fourth = collections.Counter()
    for tag, base, data in all_code_images():
        candidates = [n for n in names_by_image.get(tag, ()) if n in members and members[n]]
        # Minigames also load shared containers that the overlay does not name itself.
        if any(n.split("/")[-1].startswith("mg_") for n in candidates):
            candidates += [n for n in SHARED if n in members and members[n] and n not in candidates]
        # Language folders hold the same layout; test one to avoid five identical hits.
        stems = {}
        for name in sorted(candidates):
            stem = name.split("/")[-1].rsplit("_", 1)[0] if "/" in name else name
            current = stems.get(stem)
            if current is None or name.startswith("UK/"):
                stems[stem] = name
        files = sorted(stems.values())
        if not files:
            continue
        count = len(data) // 2
        values = struct.unpack_from(f"<{count}h", data)
        for i in range(0, count - 3):
            first, second, frame, anim = values[i:i + 4]
            if first < 0 or second < 0 or frame < 0:
                continue
            for rel in files:
                table = members[rel]
                if first not in table or second not in table or frame not in table:
                    continue
                # Both orders occur: the kinds of the two entries say which is which.
                if table[first][0].startswith("tiles") and table[second][0] == "palette":
                    tile, palette, order = first, second, "tiles-first"
                elif table[first][0] == "palette" and table[second][0].startswith("tiles"):
                    tile, palette, order = second, first, "palette-first"
                else:
                    continue
                frames = frames_of(rel, frame)
                if not frames or not any(frames):
                    continue
                bpp = frame_bpp(frames)
                if bpp is None or not table[tile][0].startswith(f"tiles{bpp}"):
                    continue
                size = len(table[tile][1]) - 4
                units = [u for u in UNITS if bytes_needed(frames, u) == size]
                if not units:
                    tally["kinds match, frames do not fit"] += 1
                    continue
                anim_kind = table[anim][0] if anim in table else ("none" if anim == -1 else "?")
                fourth[anim_kind] += 1
                # The fourth field must be an animation (or none) for the record to count.
                if anim != -1 and (anim_kind != "offsets" or frames_of(rel, anim) is not None):
                    tally["fourth field is not an animation"] += 1
                    continue
                tally["descriptors"] += 1
                tally[order] += 1
                tally[f"{order} in {'main program' if tag == 'arm9' else 'overlays'}"] += 1
                rows.append([tag, f"{base + i * 2:#010x}", rel, tile, palette, frame, anim, units[0], order])

    unique = {(r[2], r[3], r[4], r[5]) for r in rows}
    with open("sprite_tables.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image", "addr", "file", "tiles", "palette", "frames", "anim", "unit", "order"])
        w.writerows(rows)

    for key, value in tally.most_common():
        print(f"{key:36}{value}")
    print(f"distinct (file, tiles, palette, frames): {len(unique)}")
    print(f"images with descriptors: {len({r[0] for r in rows})}")
    print(f"files with descriptors: {len({r[2] for r in rows})}")
    aligned = sum(1 for r in rows if (int(r[1], 16) & 3) == 0)
    print(f"descriptors at a 4-byte boundary: {aligned} of {len(rows)}")
    print(f"+6 kinds: {fourth.most_common(8)}")
    print("\nsamples:")
    for row in rows[:14]:
        print(" ", row)
    conflicts = collections.defaultdict(set)
    for r in rows:
        conflicts[(r[2], r[5])].add((r[3], r[4]))
    multi = {k: v for k, v in conflicts.items() if len(v) > 1}
    print(f"\nframe sets with more than one (tiles, palette): {len(multi)}")
    for key, value in list(multi.items())[:8]:
        print(" ", key, sorted(value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
