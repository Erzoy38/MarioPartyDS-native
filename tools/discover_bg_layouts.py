#!/usr/bin/env python3
"""Find which table layouts hold (map, tiles, palette) indices.

For each address a code image holds as a word and that points into the same image, reads
fields of width 1, 2 and 4 at offsets 0 to 12 and looks for three consecutive fields that
name a map, a tile set and a palette in any order, in a file the image names. Prints a
histogram of layouts, so the common ones can be harvested with confidence.

Also scores gfx_screens.csv (the current rule) against bg_tables.csv.

Usage: python tools/discover_bg_layouts.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
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
                members[rel][int(parts[1])] = classify(data).split("-")[0]
    with open("file_refs.csv", newline="") as f:
        names_by_image = collections.defaultdict(set)
        for row in csv.DictReader(f):
            names_by_image[row["image"]].add(normal_name(row["name"]))

    layouts = collections.Counter()
    samples = collections.defaultdict(list)
    maps_found = collections.defaultdict(set)
    for tag, base, data in all_code_images():
        if tag == "arm9":
            continue
        files = unique_stems(n for n in names_by_image.get(tag, ()) if n in members and members[n])
        if not files:
            continue
        words = len(data) // 4
        targets = {w - base for w in struct.unpack_from(f"<{words}I", data)
                   if base <= w < base + len(data)}
        for target in sorted(targets):
            for width, code in FORMATS.items():
                if target % width:
                    continue
                for offset in range(0, 13, width):
                    start = target + offset
                    if start + width * 3 > len(data):
                        continue
                    a, b, c = struct.unpack_from(f"<3{code}", data, start)
                    if min(a, b, c) < 0 or len({a, b, c}) < 3:
                        continue
                    for rel in files:
                        table = members[rel]
                        kinds = [table.get(a), table.get(b), table.get(c)]
                        short = ["M" if k == "map" else "P" if k == "palette"
                                 else "T" if k in ("tiles4", "tiles8") else "?" for k in kinds]
                        order = "".join(short)
                        if sorted(order) != ["M", "P", "T"]:
                            continue
                        key = (width, offset, order)
                        layouts[key] += 1
                        if len(samples[key]) < 3:
                            samples[key].append((tag, f"{base + start:#x}", rel.split("/")[-1], a, b, c))
                        maps_found[key].add((rel, [a, b, c][order.index("M")]))

    print("layouts (field width, offset, order): hits, distinct maps")
    for key, count in layouts.most_common(24):
        print(f"  {str(key):24}{count:6}{len(maps_found[key]):6}  {samples[key][:2]}")

    if os.path.exists("bg_tables.csv"):
        with open("gfx_screens.csv", newline="") as f:
            mine = {r["map"]: r for r in csv.DictReader(f)}
        truth = collections.defaultdict(set)
        with open("bg_tables.csv", newline="") as f:
            for r in csv.DictReader(f):
                truth[(r["file"], int(r["map"]))].add((int(r["tiles"]), int(r["palette"])))
        tally = collections.Counter()
        for (rel, map_index), options in truth.items():
            if len(options) != 1:
                continue
            tiles, palette = next(iter(options))
            row = mine.get(f"{rel}#{map_index:03}")
            if row is None:
                tally["not in render list"] += 1
                continue
            same_tiles = row["tiles"] == f"{rel}#{tiles:03}"
            same_palette = row["palette"] == f"{rel}#{palette:03}"
            tally["maps"] += 1
            tally[f"tiles {'right' if same_tiles else 'wrong'} [{row['tiles_chosen']}]"] += 1
            tally[f"palette {'right' if same_palette else 'wrong'}"] += 1
        print("\nrule against harvested records:", dict(sorted(tally.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
