#!/usr/bin/env python3
"""Find file-name strings in the code images and which code refers to them.

A string is a run of printable ASCII starting with "data/" or naming a known ROM
file. A reference is an aligned word holding the string's address, looked for in
the image holding the string and in the main program. Writes file_refs.csv.

Usage: python tools/map_file_refs.py
"""
import collections
import csv
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import all_code_images  # noqa: E402

NAME_RE = re.compile(rb"[A-Za-z0-9_/.\-]{4,}\.(?:bin|sdat|BIN)\x00")


def main():
    images = list(all_code_images())
    with open("nds_files.csv", newline="") as f:
        rom_files = {r["path"] for r in csv.DictReader(f) if r["path"].startswith("data/")}

    words = {}
    for tag, base, data in images:
        count = len(data) // 4
        index = collections.defaultdict(list)
        for i, value in enumerate(struct.unpack_from(f"<{count}I", data)):
            index[value].append(base + i * 4)
        words[tag] = index

    rows = []
    found = set()
    per_image = collections.Counter()
    for tag, base, data in images:
        for match in NAME_RE.finditer(data):
            name = match.group()[:-1].decode("ascii")
            addr = base + match.start()
            path = name if name.startswith("data/") else f"data/{name}"
            exists = path in rom_files
            if exists:
                found.add(path)
            refs = [f"{tag}:{a:#x}" for a in words[tag].get(addr, [])]
            if not tag.startswith("ov"):
                for other, index in words.items():
                    if other != tag:
                        refs += [f"{other}:{a:#x}" for a in index.get(addr, [])]
            per_image[tag] += 1
            rows.append([tag, f"{addr:#010x}", name, exists, len(refs), " ".join(refs[:6])])

    with open("file_refs.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image", "addr", "name", "in_rom", "ref_count", "refs"])
        w.writerows(rows)

    print(f"strings: {len(rows)}, matching a ROM file: {sum(1 for r in rows if r[3])}")
    print(f"ROM files named in code: {len(found)} of {len(rom_files)}")
    print("strings per image (top 12): " + ", ".join(
        f"{k}:{v}" for k, v in per_image.most_common(12)))
    print(f"images with strings: {len(per_image)}")
    unreferenced = sum(1 for r in rows if r[4] == 0)
    print(f"strings without a direct word reference: {unreferenced}")
    missing = sorted(rom_files - found)
    print(f"ROM files never named: {len(missing)}, e.g. {missing[:12]}")

    print("\nfirst strings in the main program:")
    shown = 0
    for row in rows:
        if row[0] == "arm9" and shown < 24:
            print(f"  {row[1]} {row[2]} refs={row[4]} {row[5][:60]}")
            shown += 1
    print("\nsample strings in overlays:")
    shown = 0
    for row in rows:
        if row[0].startswith("ov") and shown < 24:
            print(f"  {row[0]} {row[1]} {row[2]} refs={row[4]}")
            shown += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
