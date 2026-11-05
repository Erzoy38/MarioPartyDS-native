#!/usr/bin/env python3
"""Decode an mpd message file: u32 count, u32 offsets[count], UTF-16LE strings.

Usage: python tools/decode_mpd.py <file> [first] [count]
       python tools/decode_mpd.py --summary <unpacked_dir>
"""
import glob
import os
import struct
import sys


def read_messages(path):
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0)[0]
    table_end = 4 + count * 4
    if count == 0 or table_end > len(data):
        raise ValueError("count does not fit")
    offsets = struct.unpack_from(f"<{count}I", data, 4)
    if offsets[0] != table_end or list(offsets) != sorted(offsets) or offsets[-1] >= len(data):
        raise ValueError("offset table is not consistent")
    messages = []
    for i, start in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < count else len(data)
        text = data[start:end].decode("utf-16-le", "replace")
        messages.append(text.split("\0")[0])
    return messages


def summary(root):
    paths = sorted(glob.glob(os.path.join(root, "data", "*", "mpd_*")))
    bad = 0
    for path in paths:
        name = os.path.relpath(path, root)
        if not os.path.isfile(path):
            continue
        try:
            messages = read_messages(path)
        except ValueError as exc:
            bad += 1
            print(f"{name}: {exc}")
            continue
        if os.sep + "UK" + os.sep in path:
            sample = messages[0][:44].replace("\n", " ")
            print(f"{os.path.basename(path):34}{len(messages):6}  {sample}")
    print(f"files={len(paths)} not_matching={bad}")


def main():
    if sys.argv[1] == "--summary":
        summary(sys.argv[2])
        return 0
    messages = read_messages(sys.argv[1])
    first = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(messages)
    for i in range(first, min(first + count, len(messages))):
        print(f"{i:4} {messages[i]!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
