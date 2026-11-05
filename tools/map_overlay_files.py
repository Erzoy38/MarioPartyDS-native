#!/usr/bin/env python3
"""Join modes.csv with the file names each overlay refers to.

Reads file_refs.csv and modes.csv, writes overlay_files.csv.
Usage: python tools/map_overlay_files.py
"""
import collections
import csv
import re
import sys

SHARED = ("mg_000", "mpds_efc", "mg_demo", "mg_pause")


def main():
    with open("file_refs.csv", newline="") as f:
        refs = list(csv.DictReader(f))
    with open("modes.csv", newline="") as f:
        modes = list(csv.DictReader(f))

    names = collections.defaultdict(list)
    for row in refs:
        if row["image"].startswith("ov"):
            names[int(row["image"][2:])].append(row["name"])

    modes_of = collections.defaultdict(list)
    for row in modes:
        modes_of[int(row["overlay"])].append(row["mode"])

    rows = []
    kinds = collections.Counter()
    for ov in range(108):
        files = sorted(set(names.get(ov, [])))
        own = sorted({m for n in files for m in re.findall(r"mg_(\d{3}[a-z]?)", n)
                      if m != "000"})
        rows.append([ov, " ".join(modes_of.get(ov, [])), " ".join(own), " ".join(files)])
        kinds["with mg id" if own else "named files only" if files else "no file names"] += 1

    with open("overlay_files.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["overlay", "modes", "mg_ids", "files"])
        w.writerows(rows)

    print(dict(kinds))
    for row in rows:
        files = row[3].replace("data/", "")
        print(f"ov{row[0]:03} {row[1]:12} mg=[{row[2]}] {files[:92]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
