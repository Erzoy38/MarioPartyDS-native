#!/usr/bin/env python3
"""What does the game's own wireless mode consist of? Strings, files and callers.

Usage: python tools/survey_wireless.py
"""
import collections
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import all_code_images  # noqa: E402

PATTERN = re.compile(rb"[ -~]{0,50}(?:WH_|WM_|WMi|WFS|WXC|MBi?_|KeyShare|DataShare|MultiBoot|\.srl|child|parent)[ -~]{0,50}",
                     re.IGNORECASE)


def main():
    found = collections.defaultdict(set)
    per_image = collections.Counter()
    for tag, _base, data in all_code_images():
        for match in PATTERN.finditer(data):
            text = match.group().decode("ascii", "replace").strip()
            found[text].add(tag)
            per_image[tag] += 1
    print("images with wireless strings:", per_image.most_common(12))
    families = collections.Counter(re.match(r"[A-Za-z]+_?", t).group() if re.match(r"[A-Za-z]+_?", t) else "?"
                                   for t in found)
    print("string families:", families.most_common(20))
    for text in sorted(found):
        if re.search(r"KeyShare|DataShare|WFS|WXC|srl|MultiBoot|child|parent", text, re.IGNORECASE):
            print(f"  {sorted(found[text])[0]:6} {text[:96]}")

    print("\nROM files named for wireless modes, or not named at all:")
    with open("nds_files.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        path = row["path"]
        if re.search(r"UK/(mb|download)|\.srl|unnamed", path):
            print(f"  {path:44}{row['size']:>10}")
    print("\nfiles at the data root that are not mg_, bmap or collection:")
    for row in rows:
        path = row["path"]
        if path.count("/") == 1 and path.startswith("data/") \
                and not re.match(r"data/(mg_|bmap|collection)", path):
            print(f"  {path:44}{row['size']:>10}")

    print("\nmodes around the wireless menu (overlay_files.csv):")
    with open("overlay_files.csv", newline="") as f:
        for row in csv.DictReader(f):
            if re.search(r"download_play|mbgui|mbturn|mbpause", row["files"]):
                print(f"  ov{int(row['overlay']):03} modes={row['modes']} {row['files'][:80]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
