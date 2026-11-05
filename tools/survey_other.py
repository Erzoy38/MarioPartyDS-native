#!/usr/bin/env python3
"""Look for structure in leaves classified as 'other' by asset_walk.py.

Tests size formulas from the first bytes and reports how many leaves each explains.
Usage: python tools/survey_other.py [name_substring]
"""
import collections
import csv
import sys


def main():
    with open("assets.csv", newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["kind"] == "other"]
    if len(sys.argv) > 1:
        rows = [r for r in rows if sys.argv[1] in r["name"]]
    print(f"other leaves: {len(rows)}")

    tests = collections.Counter()
    tested_bytes = collections.Counter()
    for row in rows:
        size = int(row["size"])
        head = bytes.fromhex(row["head"])
        if len(head) < 8:
            tests["too short"] += 1
            continue
        hits = []
        if size == 256 * 192 * 2:
            hits.append("full screen bgr555")
        if size == 512 * 2 or size == 256 * 2 or size == 32:
            hits.append("raw palette size")
        if size % 32 == 0:
            hits.append("multiple of 32")
        if (size - 4) % 32 == 0:
            hits.append("4 + multiple of 32")
        if (size - 8) % 32 == 0:
            hits.append("8 + multiple of 32")
        w = int.from_bytes(head[0:2], "little")
        h = int.from_bytes(head[2:4], "little")
        if w and h and size == 4 + w * h // 2:
            hits.append("u16 w, u16 h, 4bpp")
        if w and h and size == 4 + w * h:
            hits.append("u16 w, u16 h, 8bpp")
        if w and h and size == 8 + w * h // 2:
            hits.append("u16 w, u16 h, 8 head, 4bpp")
        if w and h and size == 8 + w * h:
            hits.append("u16 w, u16 h, 8 head, 8bpp")
        if w and h and size == 8 + w * h * 2:
            hits.append("u16 w, u16 h, 8 head, 16bpp")
        word = int.from_bytes(head[0:4], "little")
        if word and word < size and (size - word) < 64:
            hits.append("u32 near size")
        if word and word * 4 + 4 < size and int.from_bytes(head[4:8], "little") == word * 4 + 4:
            hits.append("offset table count*4+4")
        if word and word * 4 < size and int.from_bytes(head[4:8], "little") == word * 4:
            hits.append("offset table count*4")
        for hit in hits or ["unexplained"]:
            tests[hit] += 1
            tested_bytes[hit] += size
    for name, count in tests.most_common():
        print(f"  {name:32}{count:7}{tested_bytes[name]:11}")

    print("\nmost common first 4 bytes:")
    heads = collections.Counter(r["head"][:8] for r in rows)
    samples = {}
    for r in rows:
        samples.setdefault(r["head"][:8], r)
    for key, count in heads.most_common(16):
        s = samples[key]
        print(f"  {key} {count:6}  {s['name'][:44]:44} {s['size']}")

    print("\nmost common sizes:")
    for size, count in collections.Counter(int(r["size"]) for r in rows).most_common(14):
        print(f"  {size:8}{count:6}")

    print("\nlargest by folder stem:")
    stems = collections.Counter()
    for r in rows:
        stems[r["name"].split("#")[0].split("/")[-1].rsplit("_", 2)[0][:16]] += int(r["size"])
    print("  " + ", ".join(f"{k}:{v}" for k, v in stems.most_common(14)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
