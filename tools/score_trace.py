#!/usr/bin/env python3
"""Score my map pairing against what the running game loaded (trace_loads.csv).

The pairing is inferred from call order: a map goes with the tile set last loaded for the
same engine and layer, and the palette last loaded for the same engine. Calls close in
time from the same screen setup are what this trusts, so the result is strong evidence
but not proof. Compares by leaf content, using the same names in any language folder.

Usage: python tools/score_trace.py
Writes trace_pairs.csv.
"""
import collections
import csv
import sys

WINDOW = 3.0  # seconds; loads further apart are not treated as one screen setup


def base(name):
    """Drop the language folder, so FR/x#001 and UK/x#001 compare equal."""
    return name.split("/")[-1]


def main():
    with open("trace_loads.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    with open("gfx_screens.csv", newline="") as f:
        mine = {base(r["map"]): r for r in csv.DictReader(f)}

    kinds = collections.Counter((r["kind"], r["match"]) for r in rows)
    print("calls by kind and match:")
    for key, value in sorted(kinds.items()):
        print(f"  {key}: {value}")
    print("overlays loaded:", [r["names"] for r in rows if r["kind"] == "overlay"])

    tally = collections.Counter()
    pairs = []
    wrong = []
    tiles_seen, palette_seen = {}, {}
    for row in rows:
        moment = float(row["time"])
        names = row["names"].split()
        engine, layer = row["engine"], row["layer_or_arg"]
        if row["func"] == "load_tiles":
            tiles_seen[(engine, layer)] = (moment, names)
        elif row["func"] == "load_palette":
            palette_seen[engine] = (moment, names)
        elif row["kind"] == "map":
            if not names:
                tally["map not identified"] += 1
                continue
            tile = tiles_seen.get((engine, layer))
            palette = palette_seen.get(engine)
            for name in names[:1]:
                key = base(name)
                row_mine = mine.get(key)
                if row_mine is None:
                    tally["map not in my render list"] += 1
                    continue
                tally["maps"] += 1
                source = row_mine["source"]
                if tile and moment - tile[0] <= WINDOW and tile[1]:
                    same = base(row_mine["tiles"]) in {base(n) for n in tile[1]}
                    tally[f"tiles {'agree' if same else 'differ'} [{source}]"] += 1
                else:
                    same = None
                    tally["tiles unknown"] += 1
                if palette and moment - palette[0] <= WINDOW and palette[1]:
                    same_p = base(row_mine["palette"]) in {base(n) for n in palette[1]}
                    tally[f"palette {'agree' if same_p else 'differ'} [{source}]"] += 1
                else:
                    same_p = None
                    tally["palette unknown"] += 1
                pairs.append([key, tile[1][0] if tile and tile[1] else "",
                              palette[1][0] if palette and palette[1] else "", source, same, same_p])
                if same is False or same_p is False:
                    wrong.append((key, row_mine["tiles"], row_mine["palette"],
                                  tile[1][:1] if tile else "", palette[1][:1] if palette else ""))
    with open("trace_pairs.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["map", "tiles_seen", "palette_seen", "my_source", "tiles_agree", "palette_agree"])
        w.writerows(pairs)
    print("\nmy pairing against the trace:")
    for key, value in sorted(tally.items()):
        print(f"  {key}: {value}")
    print(f"\ndistinct maps seen: {len({p[0] for p in pairs})}")
    for item in wrong[:16]:
        print("  differs:", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
