#!/usr/bin/env python3
"""Walk every data file through all container and compression levels, classify leaves.

Classification rules (each checks the exact file size, so false hits are unlikely):
  map     u16 bytes, u8 w, u8 h; bytes == w*h*2; size == 4 + bytes
  palette u16 bytes, u16 colors; bytes == colors*2; size == 4 + bytes
  tiles4  u32 with count in bits 8+; low byte = format; size == 4 + count*32
  tiles8  same, size == 4 + count*64
  hbdf, texs, sdat by signature; text = mpd file layout

Writes assets.csv. Usage: python tools/asset_walk.py [unpacked_dir]
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decompress import DecodeError, huff_decompress, lz10_decompress, parse_container  # noqa: E402


def classify(data):
    size = len(data)
    if size < 4:
        return "tiny"
    if data[:4] == b"HBDF":
        return "hbdf-" + data[8:12].decode("latin-1")
    if data[:4] == b"TEXS":
        return "texs"
    if data[:4] == b"SDAT":
        return "sdat"
    if size == 256 * 192 * 2:
        return "bitmap16"
    a, b = struct.unpack_from("<HH", data, 0)
    w, h = data[2], data[3]
    if a == w * h * 2 and size == 4 + a and a:
        return "map"
    if a == b * 2 and size == 4 + a and a:
        return "palette"
    word = struct.unpack_from("<I", data, 0)[0]
    count = word >> 8
    if count and size == 4 + count * 32:
        return f"tiles4-{word & 0xFF:02x}"
    if count and size == 4 + count * 64:
        return f"tiles8-{word & 0xFF:02x}"
    if size >= 8 and word and size >= 4 + word * 4 \
            and struct.unpack_from("<I", data, 4)[0] == word * 4:
        return "offsets"
    if size >= 8 and word and size >= 4 + word * 4 \
            and struct.unpack_from("<I", data, 4)[0] == word * 4 + 4:
        return "text"
    return "other"


def expand(data):
    """Undo compression if the data decodes exactly; returns (data, method)."""
    if data[:1] == b"\x10":
        try:
            out, used = lz10_decompress(data)
            if len(data) - used < 4:
                return out, "lz10"
        except DecodeError:
            pass
    if data[:1] == b"\x28":
        try:
            out, used = huff_decompress(data)
            if len(data) - used < 4:
                return out, "huff"
        except DecodeError:
            pass
    return data, ""


def unwrap(data):
    """Remove the 4-byte wrapper: u32 (inner size << 8), low byte 0."""
    if len(data) >= 8:
        word = struct.unpack_from("<I", data, 0)[0]
        if word & 0xFF == 0 and word >> 8 == len(data) - 4:
            return data[4:]
    return data


def walk(data, name, depth=0):
    """Yield (name, data, depth) for every leaf below this blob."""
    data, _method = expand(data)
    data = unwrap(data)
    entries, _why = parse_container(data)
    if entries is None or depth > 4:
        yield name, data, depth
        return
    for i, (start, length) in enumerate(entries):
        yield from walk(data[start:start + length], f"{name}#{i:03}", depth + 1)


def rom_files(root):
    base = os.path.join(root, "data")
    for folder, dirs, names in os.walk(base):
        dirs[:] = sorted(d for d in dirs if not d.endswith(".d"))
        for name in sorted(names):
            path = os.path.join(folder, name)
            yield os.path.relpath(path, base).replace(os.sep, "/"), path


def container_original(root, rel):
    """decompress.py split some containers already; rebuild the walk from their parts."""
    folder = os.path.join(root, "data", *rel.split("/")) + ".d"
    return folder if os.path.isdir(folder) else None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "unpacked"
    kinds = collections.Counter()
    sizes = collections.Counter()
    rows = []
    seen = set()
    for folder, dirs, names in os.walk(os.path.join(root, "data")):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, os.path.join(root, "data")).replace(os.sep, "/")
            with open(path, "rb") as f:
                blob = f.read()
            for leaf, data, depth in walk(blob, rel):
                kind = classify(data)
                kinds[kind] += 1
                sizes[kind] += len(data)
                rows.append([leaf, kind, len(data), data[:8].hex()])
                seen.add(leaf)

    with open("assets.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "kind", "size", "head"])
        w.writerows(rows)

    total = sum(sizes.values())
    print(f"leaves={len(rows)} bytes={total}")
    for kind, count in kinds.most_common():
        print(f"  {kind:14}{count:6}{sizes[kind]:12}  {100 * sizes[kind] / total:5.1f}%")

    # How often does a map, palette and tile set appear together in one parent?
    groups = collections.defaultdict(list)
    for name, kind, _size, _head in rows:
        parent = name.rsplit("#", 1)[0] if "#" in name else name.rsplit("/", 1)[0]
        groups[parent].append(kind.split("-")[0])
    patterns = collections.Counter()
    for parent, members in groups.items():
        patterns[" ".join(members[:6])] += 1
    print("\nmember patterns (first 6, top 14):")
    for pattern, count in patterns.most_common(14):
        print(f"  {count:5}  {pattern}")

    print("\nlargest 'other' leaves:")
    others = sorted((r for r in rows if r[1] == "other"), key=lambda r: -r[2])[:14]
    for row in others:
        print(f"  {row[0][:52]:52}{row[2]:9}  {row[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
