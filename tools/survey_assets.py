#!/usr/bin/env python3
"""Survey unpacked data files: signatures, HBDF header fields, container layouts.

Usage: python tools/survey_assets.py [unpacked_dir]
"""
import collections
import os
import struct
import sys


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "unpacked"
    base = os.path.join(root, "data")
    paths = []
    for folder, _, names in os.walk(base):
        for name in names:
            paths.append(os.path.join(folder, name))

    hbdf = []
    by_folder = collections.Counter()
    head_words = collections.Counter()
    samples = {}
    for path in sorted(paths):
        with open(path, "rb") as f:
            data = f.read()
        rel = os.path.relpath(path, base)
        if data[:4] == b"HBDF":
            hbdf.append((rel, data))
            by_folder[os.path.dirname(rel).split(os.sep)[0] if os.sep in rel else "(root)"] += 1
        else:
            key = data[:4].hex()
            head_words[key] += 1
            samples.setdefault(key, rel)

    print(f"files={len(paths)} hbdf={len(hbdf)}")
    print(f"hbdf by top folder (top 12): {by_folder.most_common(12)}")

    print("\nhbdf samples, first 48 bytes:")
    step = max(len(hbdf) // 14, 1)
    for rel, data in hbdf[::step]:
        print(f"  {rel[:44]:44} {len(data):8}  {data[:48].hex(' ')}")

    field4 = collections.Counter(struct.unpack_from("<I", d, 4)[0] for _, d in hbdf)
    field8 = collections.Counter(struct.unpack_from("<I", d, 8)[0] for _, d in hbdf)
    print(f"\nhbdf +4 values (top 10): {field4.most_common(10)}")
    print(f"hbdf +8 values (top 10): {field8.most_common(10)}")
    size_match = sum(1 for _, d in hbdf if struct.unpack_from("<I", d, 4)[0] == len(d))
    print(f"hbdf +4 equals file size: {size_match}")

    print("\nmost common non-hbdf first words (top 25):")
    for key, count in head_words.most_common(25):
        print(f"  {key} {count:5}  {samples[key]}")

    print("\nsample files by name (largest non-hbdf):")
    sized = sorted(paths, key=os.path.getsize, reverse=True)
    shown = 0
    for path in sized:
        with open(path, "rb") as f:
            head = f.read(16)
        if head[:4] != b"HBDF":
            print(f"  {os.path.relpath(path, base)[:46]:46} {os.path.getsize(path):8}  {head.hex(' ')}")
            shown += 1
            if shown == 18:
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
