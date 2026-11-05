#!/usr/bin/env python3
"""Measure a looser descriptor match before trusting it.

Loose: tile set and palette in either order, then a frame set that fits the tiles exactly,
with no condition on the fourth field. Each loose hit for a frame set that already has a
strict descriptor is scored: does it name the same tiles and palette? That rate estimates
how far loose hits can be trusted for frame sets with no strict descriptor.

Writes sprite_tables_loose.csv. Usage: python tools/check_weak_descriptors.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from harvest_sprite_tables import SHARED, normal_name  # noqa: E402
from nds_image import all_code_images  # noqa: E402
from render_screen import source_files  # noqa: E402
from render_sprites import UNITS, bytes_needed, frame_bpp, parse_frames  # noqa: E402


def main():
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

    strict = collections.defaultdict(set)
    with open("sprite_tables.csv", newline="") as f:
        for row in csv.DictReader(f):
            if row["image"] != "arm9":
                strict[(row["file"], int(row["frames"]))].add((int(row["tiles"]), int(row["palette"])))

    cache = {}

    def frames_of(rel, index):
        key = (rel, index)
        if key not in cache:
            kind, data = members[rel][index]
            cache[key] = parse_frames(data) if kind == "offsets" else None
        return cache[key]

    loose = collections.defaultdict(lambda: collections.defaultdict(list))
    for tag, base, data in all_code_images():
        if tag == "arm9":
            continue
        candidates = [n for n in names_by_image.get(tag, ()) if n in members and members[n]]
        if any(n.split("/")[-1].startswith("mg_") for n in candidates):
            candidates += [n for n in SHARED if n in members and members[n] and n not in candidates]
        stems = {}
        for name in sorted(candidates):
            stem = name.split("/")[-1]
            if stem not in stems or name.startswith("UK/"):
                stems[stem] = name
        files = sorted(stems.values())
        if not files:
            continue
        count = len(data) // 2
        values = struct.unpack_from(f"<{count}h", data)
        for i in range(count - 2):
            first, second, frame = values[i:i + 3]
            if first < 0 or second < 0 or frame < 0:
                continue
            for rel in files:
                table = members[rel]
                if first not in table or second not in table or frame not in table:
                    continue
                if table[first][0].startswith("tiles") and table[second][0] == "palette":
                    tile, palette = first, second
                elif table[first][0] == "palette" and table[second][0].startswith("tiles"):
                    tile, palette = second, first
                else:
                    continue
                frames = frames_of(rel, frame)
                if not frames or not any(frames):
                    continue
                bpp = frame_bpp(frames)
                if bpp is None or not table[tile][0].startswith(f"tiles{bpp}"):
                    continue
                size = len(table[tile][1]) - 4
                if not any(bytes_needed(frames, u) == size for u in UNITS):
                    continue
                loose[(rel, frame)][(tile, palette)].append(f"{tag}:{base + i * 2:#x}")

    tally = collections.Counter()
    open_rows = []
    for key, options in loose.items():
        if key in strict:
            for pairing in options:
                tally["loose pairing agrees with strict" if pairing in strict[key]
                      else "loose pairing not among strict"] += 1
        else:
            tally["frame sets only found loosely"] += 1
            tally[f"  with {min(len(options), 3)}{'+' if len(options) >= 3 else ''} option(s)"] += 1
            rel, frame = key
            if len(options) == 1:
                (tile, palette), where = next(iter(options.items()))
                open_rows.append([rel, frame, tile, palette, where[0]])
    for key, value in sorted(tally.items()):
        print(f"{key:40}{value}")
    with open("sprite_tables_loose.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "frames", "tiles", "palette", "where"])
        w.writerows(open_rows)
    print("\nsamples of frame sets found only loosely with one option:")
    for row in open_rows[:16]:
        print(" ", row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
