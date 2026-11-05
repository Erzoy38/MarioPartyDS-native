#!/usr/bin/env python3
"""Second tier of background records: any field order, but exact fit and tied to code.

For each address held as a word in an overlay (pointing into that overlay), reads fields
of width 1, 2 and 4 at offsets 0 to 12. Three consecutive fields count if they name a map,
a tile set and a palette in any order, in a file the overlay names, and the map's highest
tile index + 1 equals the tile count.

Before these can be used, they are scored against tier 1 (bg_tables.csv).
Writes bg_tables_generic.csv. Usage: python tools/harvest_bg_generic.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from gfx import parse_map  # noqa: E402
from harvest_bg_tables import unique_stems  # noqa: E402
from harvest_sprite_tables import normal_name  # noqa: E402
from nds_image import all_code_images  # noqa: E402
from render_screen import source_files  # noqa: E402

FORMATS = {1: "b", 2: "h", 4: "i"}


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

    tops = {}

    def top_index(rel, index):
        key = (rel, index)
        if key not in tops:
            tops[key] = max(e & 0x3FF for e in parse_map(members[rel][index][1])[2])
        return tops[key]

    rows = []
    pairings = collections.defaultdict(lambda: collections.defaultdict(list))
    for tag, base, data in all_code_images():
        if tag == "arm9":
            continue
        files = unique_stems(n for n in names_by_image.get(tag, ()) if n in members and members[n])
        if not files:
            continue
        words = len(data) // 4
        targets = {w - base for w in struct.unpack_from(f"<{words}I", data)
                   if base <= w < base + len(data)}
        seen = set()
        for target in sorted(targets):
            for width, code in FORMATS.items():
                if target % width:
                    continue
                for offset in range(0, 13, width):
                    start = target + offset
                    if (start, width) in seen or start + width * 3 > len(data):
                        continue
                    seen.add((start, width))
                    fields = struct.unpack_from(f"<3{code}", data, start)
                    if min(fields) < 0 or len(set(fields)) < 3:
                        continue
                    for rel in files:
                        table = members[rel]
                        if any(v not in table for v in fields):
                            continue
                        kinds = {table[v][0].split("-")[0]: v for v in fields}
                        tile_key = "tiles4" if "tiles4" in kinds else "tiles8"
                        if set(kinds) != {"map", "palette", tile_key}:
                            continue
                        map_, tiles, palette = kinds["map"], kinds[tile_key], kinds["palette"]
                        count = struct.unpack_from("<I", table[tiles][1], 0)[0] >> 8
                        if top_index(rel, map_) + 1 != count:
                            continue
                        order = "".join("M" if v == map_ else "T" if v == tiles else "P" for v in fields)
                        rows.append([tag, f"{base + start:#010x}", rel, map_, tiles, palette, width, order])
                        pairings[(rel, map_)][(tiles, palette)].append(tag)

    with open("bg_tables_generic.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image", "addr", "file", "map", "tiles", "palette", "width", "order"])
        w.writerows(rows)

    print(f"records: {len(rows)}, maps: {len(pairings)}, files: {len({k[0] for k in pairings})}")
    single = {k: next(iter(v)) for k, v in pairings.items() if len(v) == 1}
    print(f"maps with one pairing: {len(single)}, with several: {len(pairings) - len(single)}")
    orders = collections.Counter((r[6], r[7]) for r in rows)
    print(f"width and order: {orders.most_common(10)}")

    tier1 = collections.defaultdict(set)
    with open("bg_tables.csv", newline="") as f:
        for r in csv.DictReader(f):
            tier1[(r["file"], int(r["map"]))].add((int(r["tiles"]), int(r["palette"])))
    tally = collections.Counter()
    for key, options in tier1.items():
        if key in single:
            agree = single[key] in options
            tally["tier 2 agrees with tier 1" if agree else "tier 2 disagrees with tier 1"] += 1
            if not agree:
                print(f"  disagrees: {key} tier2={single[key]} tier1={sorted(options)}")
        elif key in pairings:
            tally["tier 2 ambiguous on a tier 1 map"] += 1
        else:
            tally["tier 2 has nothing for a tier 1 map"] += 1
    print(dict(tally))
    new = [k for k in single if k not in tier1]
    print(f"new maps from tier 2: {len(new)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
