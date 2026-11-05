#!/usr/bin/env python3
"""Print or extract a leaf from asset_walk.py by name.

Usage: python tools/dump_asset.py <leaf name> [length]    hex dump the leaf
       python tools/dump_asset.py --list <file> [limit]   list a file's leaves

Leaf names look like data_path#003#001 as in assets.csv.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify, walk  # noqa: E402

ROOT = "unpacked"


def load(rel):
    with open(os.path.join(ROOT, "data", *rel.split("/")), "rb") as f:
        return f.read()


def leaves(rel):
    return list(walk(load(rel), rel))


def main():
    if sys.argv[1] == "--list":
        limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
        for name, data, _depth in leaves(sys.argv[2])[:limit]:
            print(f"{name:44}{classify(data):12}{len(data):8}  {data[:16].hex(' ')}")
        return 0
    name = sys.argv[1]
    length = int(sys.argv[2], 0) if len(sys.argv) > 2 else 96
    for leaf, data, _depth in leaves(name.split("#")[0]):
        if leaf == name:
            print(f"{leaf}: {classify(data)} {len(data)} bytes")
            for i in range(0, min(length, len(data)), 16):
                row = data[i:i + 16]
                print(f"{i:06x}  {row.hex(' '):47}")
            return 0
    print("not found")
    return 1


if __name__ == "__main__":
    sys.exit(main())
