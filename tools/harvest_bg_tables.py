#!/usr/bin/env python3
"""Harvest background records from overlays: the game's own map/tiles/palette pairing.

Seen in overlay 1 (loader 0x021697A8, table 0x0216E618): records of s8 fields
  +0 palette, +1 tiles, +2 map, +3 engine, +7 layer; 10 bytes each.

Byte patterns match easily by chance, so a hit must be tied to the code: its address must
be a word in the same image (a literal or a pointer), or it must sit 10 bytes from such a
record in an unbroken chain. It also needs the right kinds in +0, +1, +2, engine 0 or 1
and layer 0 to 3. Map indices may exceed the tile set: overlay 1 does this on purpose.
Files the overlay names itself take priority over shared minigame files.

Writes bg_tables.csv. Usage: python tools/harvest_bg_tables.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from gfx import parse_map  # noqa: E402
from harvest_sprite_tables import SHARED, normal_name  # noqa: E402
from nds_image import all_code_images  # noqa: E402
from render_screen import source_files  # noqa: E402

STRIDE = 10


def unique_stems(names):
    stems = {}
    for name in sorted(names):
        stem = name.split("/")[-1]
        if stem not in stems or name.startswith("UK/"):
            stems[stem] = name
    return sorted(stems.values())


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

    def matches(rel, data, i):
        table = members[rel]
        palette, tiles, map_ = data[i], data[i + 1], data[i + 2]
        return (palette in table and tiles in table and map_ in table
                and table[palette][0] == "palette" and table[tiles][0].startswith("tiles")
                and table[map_][0] == "map")

    tally = collections.Counter()
    rows = []
    for tag, base, data in all_code_images():
        if tag == "arm9":
            continue
        own = [n for n in names_by_image.get(tag, ()) if n in members and members[n]]
        shared = []
        if any(n.split("/")[-1].startswith("mg_") for n in own):
            shared = [n for n in SHARED if n in members and members[n] and n not in own]
        own, shared = unique_stems(own), unique_stems(shared)
        if not own and not shared:
            continue
        words = len(data) // 4
        referenced = {w - base for w in struct.unpack_from(f"<{words}I", data)
                      if base <= w < base + len(data)}

        def valid(i):
            return i + 8 <= len(data) and data[i + 3] <= 1 and data[i + 7] <= 3

        for i in range(len(data) - 8):
            if not valid(i):
                continue
            hit_files = [r for r in own if matches(r, data, i)]
            source = "own"
            if not hit_files:
                hit_files = [r for r in shared if matches(r, data, i)]
                source = "shared"
            if not hit_files:
                continue
            tally["kinds, engine and layer match"] += 1
            # Direct reference, or a chain of valid records back to one.
            tied = i in referenced
            how = "direct" if tied else ""
            j = i
            while not tied and j - STRIDE >= 0 and valid(j - STRIDE) \
                    and any(matches(r, data, j - STRIDE) for r in hit_files):
                j -= STRIDE
                if j in referenced:
                    tied = True
                    how = "chain"
            if not tied:
                continue
            tally[f"tied to code: {how}"] += 1
            tally[f"file: {source}"] += 1
            if len(hit_files) > 1:
                tally["record fits several files"] += 1
            for rel in hit_files:
                table = members[rel]
                palette, tiles, map_ = data[i], data[i + 1], data[i + 2]
                count = struct.unpack_from("<I", table[tiles][1], 0)[0] >> 8
                top = max(e & 0x3FF for e in parse_map(table[map_][1])[2])
                rows.append([tag, f"{base + i:#010x}", rel, map_, tiles, palette, data[i + 3],
                             data[i + 7], top, count, how, source])

    with open("bg_tables.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image", "addr", "file", "map", "tiles", "palette", "engine", "layer",
                    "max_index", "tile_count", "tied", "source"])
        w.writerows(rows)
    for key, value in tally.most_common():
        print(f"{key:36}{value}")
    print(f"rows: {len(rows)}, distinct (file, map): {len({(r[2], r[3]) for r in rows})}")
    print(f"images: {len({r[0] for r in rows})}, files: {len({r[2] for r in rows})}")
    exceed = sum(1 for r in rows if r[8] >= r[9])
    exact = sum(1 for r in rows if r[8] + 1 == r[9])
    print(f"exact fit: {exact}, index beyond the tile set: {exceed}")
    print("\noverlay 1:")
    for row in rows:
        if row[0] == "ov001":
            print(" ", row[1:])
    conflicts = collections.defaultdict(set)
    for r in rows:
        conflicts[(r[2], r[3])].add((r[4], r[5]))
    multi = {k: v for k, v in conflicts.items() if len(v) > 1}
    print(f"\nmaps with several pairings: {len(multi)}")
    for key, value in list(multi.items())[:10]:
        print(" ", key, sorted(value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
