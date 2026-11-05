#!/usr/bin/env python3
"""Render tilemaps and direct-colour bitmaps from the data files to gfx/.

Pairing rule under test: within one parent container, a run of maps uses the next
palette and the next tile set after it, in either order. The result is measured:
each render reports tile indices and colours that fall outside what was paired.

Usage: python tools/render_screen.py [--only substring] [--limit N]
Writes gfx/screen_*.png and gfx_screens.csv. Identical images are written once.
"""
import argparse
import collections
import csv
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify, walk  # noqa: E402
from gfx import parse_map, parse_palette, render_bitmap16, render_map, write_png  # noqa: E402

ROOT = os.path.join("unpacked", "data")


def source_files():
    """Yield (name, leaf iterator) for files and for containers split into .d folders."""
    for folder, dirs, names in os.walk(ROOT):
        dirs.sort()
        rel = os.path.relpath(folder, ROOT).replace(os.sep, "/")
        if folder.endswith(".d"):
            name = rel[:-2]
            yield name, split_container_leaves(folder, name)
            continue
        for file_name in sorted(names):
            path = os.path.join(folder, file_name)
            name = os.path.relpath(path, ROOT).replace(os.sep, "/")
            yield name, file_leaves(path, name)


def file_leaves(path, name):
    with open(path, "rb") as f:
        yield from walk(f.read(), name)


def split_container_leaves(folder, name):
    for file_name in sorted(os.listdir(folder)):
        with open(os.path.join(folder, file_name), "rb") as f:
            yield from walk(f.read(), f"{name}#{file_name.split('.')[0]}")


def runs_of(positions):
    """Split sorted positions into runs of consecutive numbers: {position: (run, index)}."""
    result = {}
    run = []
    for pos in positions + [None]:
        if run and (pos is None or pos != run[-1] + 1):
            for index, member in enumerate(run):
                result[member] = (tuple(run), index)
            run = []
        if pos is not None:
            run.append(pos)
    return result


def pair_maps(items):
    """items: ordered (name, data, kind). Yields (map, palette, tiles, how).

    Tiles: the set with exactly max_index + 1 tiles, nearest to the map. Otherwise the
    nearest set large enough ("fallback"). Palette: from the palette run nearest the
    tiles, at the same position in its run as the tiles have in theirs when the runs
    are the same length, otherwise the palette nearest to the map.
    """
    tiles_at = [i for i, it in enumerate(items) if it[2].startswith("tiles")]
    palette_at = [i for i, it in enumerate(items) if it[2] == "palette"]
    if not tiles_at or not palette_at:
        return
    tile_runs = runs_of(tiles_at)
    palette_runs = runs_of(palette_at)
    counts = {i: struct.unpack_from("<I", items[i][1], 0)[0] >> 8 for i in tiles_at}
    for pos, item in enumerate(items):
        if item[2] != "map":
            continue
        entries = parse_map(item[1])[2]
        highest = max(e & 0x3FF for e in entries)
        exact = [i for i in tiles_at if counts[i] == highest + 1]
        big_enough = [i for i in tiles_at if counts[i] > highest]
        how = "exact" if exact else "fallback"
        pool = exact or big_enough or tiles_at
        tile_pos = min(pool, key=lambda i: (abs(i - pos), i < pos))

        run, index = tile_runs[tile_pos]
        run_starts = {r[0][0]: r[0] for r in palette_runs.values()}
        nearest = min(run_starts.values(),
                      key=lambda r: min(abs(r[0] - tile_pos), abs(r[-1] - tile_pos)))
        if len(nearest) == len(run):
            palette_pos = nearest[index]
        else:
            palette_pos = min(palette_at, key=lambda i: (abs(i - pos), i < pos))
        yield item, items[palette_pos], items[tile_pos], how


def load_record_truth():
    """Pairings found in code: {(file stem, map): (tiles, palette, source)}.

    Tier 1 (bg_tables.csv) matches the record layout read from overlay 1's loader. Tier 2
    (bg_tables_generic.csv) allows any field order but requires an exact fit. Tier 2 agreed
    with tier 1 on 13 of 14 shared maps, so it is kept apart as "probable". A map with
    several different pairings in a tier is left to the next tier or the rule.
    """
    truth = {}
    for path, source in (("bg_tables_generic.csv", "probable"), ("bg_tables.csv", "record")):
        if not os.path.exists(path):
            continue
        options = collections.defaultdict(set)
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                key = (row["file"].split("/")[-1], int(row["map"]))
                options[key].add((int(row["tiles"]), int(row["palette"])))
        for key, pairs in options.items():
            if len(pairs) == 1:
                tiles, palette = next(iter(pairs))
                truth[key] = (tiles, palette, source)  # tier 1 is read last and wins
    return truth


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only")
    ap.add_argument("--limit", type=int)
    args = ap.parse_args()
    os.makedirs("gfx", exist_ok=True)

    totals = collections.Counter()
    rows = []
    seen = set()
    written = 0
    truth = load_record_truth()
    for rel, leaves in source_files():
        if args.only and args.only not in rel:
            continue
        groups = collections.defaultdict(list)
        for name, data, _depth in leaves:
            parent = name.rsplit("#", 1)[0]
            groups[parent].append((name, data, classify(data)))
        for parent, items in groups.items():
            maps_total = sum(1 for i in items if i[2] == "map")
            paired = 0
            by_index = {int(i[0].rsplit("#", 1)[1]): i for i in items if "#" in i[0]}
            top_level = parent.count("#") == 0
            stem = parent.split("/")[-1]
            for map_item, pal_item, tile_item, how in pair_maps(items):
                paired += 1
                totals[f"tiles chosen: {how}"] += 1
                source = "rule"
                known = truth.get((stem, int(map_item[0].rsplit("#", 1)[1]))) if top_level else None
                if known and known[0] in by_index and known[1] in by_index:
                    true_tile, true_palette = by_index[known[0]], by_index[known[1]]
                    if true_tile[2].startswith("tiles") and true_palette[2] == "palette":
                        totals[f"pairing from code: {known[2]}"] += 1
                        same_t = true_tile[1] == tile_item[1]
                        same_p = true_palette[1] == pal_item[1]
                        totals[f"  rule vs {known[2]}: tiles {'same' if same_t else 'differ'}"] += 1
                        totals[f"  rule vs {known[2]}: palette {'same' if same_p else 'differ'}"] += 1
                        test = {}
                        render_map(map_item[1], true_tile[1], parse_palette(true_palette[1]), test)
                        if known[2] == "probable" and test["bad_color"]:
                            # Colours outside the palette: this tier 2 pairing cannot be right.
                            totals["probable rejected: colours out of range"] += 1
                        else:
                            tile_item, pal_item, source = true_tile, true_palette, known[2]
                stats = {}
                width, height, rgba = render_map(map_item[1], tile_item[1],
                                                 parse_palette(pal_item[1]), stats)
                totals["maps rendered"] += 1
                if stats["bad_tile"] or stats["bad_color"]:
                    totals["maps with out-of-range values"] += 1
                else:
                    totals["maps fully in range"] += 1
                totals[f"bpp{stats['bpp']}"] += 1
                digest = hashlib.sha1(rgba).hexdigest()
                file_name = ""
                if digest not in seen and any(rgba[3::4]):
                    seen.add(digest)
                    file_name = "gfx/screen_" + map_item[0].replace("/", "_").replace("#", "_") + ".png"
                    if args.limit is None or written < args.limit:
                        write_png(file_name, width, height, rgba)
                        written += 1
                rows.append([map_item[0], tile_item[0], pal_item[0], width, height, stats["bpp"],
                             stats["bad_tile"], stats["bad_color"], stats["max_index"],
                             stats["tiles"], file_name, how, source])
            totals["maps without a pair"] += maps_total - paired
            for name, data, kind in items:
                if kind == "bitmap16":
                    width, height, rgba = render_bitmap16(data)
                    digest = hashlib.sha1(rgba).hexdigest()
                    totals["bitmaps"] += 1
                    if digest not in seen and any(rgba[3::4]):
                        seen.add(digest)
                        file_name = "gfx/screen_" + name.replace("/", "_").replace("#", "_") + ".png"
                        write_png(file_name, width, height, rgba)
                        written += 1

    with open("gfx_screens.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["map", "tiles", "palette", "width", "height", "bpp", "bad_tile",
                    "bad_color", "max_index", "tile_count", "png", "tiles_chosen", "source"])
        w.writerows(rows)
    for key, value in totals.most_common():
        print(f"{key:34}{value}")
    print(f"{'unique images written':34}{written}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
