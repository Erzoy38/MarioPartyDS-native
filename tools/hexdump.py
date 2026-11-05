#!/usr/bin/env python3
"""Hex dump of part of a file.

Usage: python tools/hexdump.py <file> [offset] [length]
"""
import sys


def main():
    path = sys.argv[1]
    offset = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 256
    with open(path, "rb") as f:
        data = f.read()
    print(f"{path}: {len(data)} bytes ({len(data):#x})")
    chunk = data[offset:offset + length]
    for i in range(0, len(chunk), 16):
        row = chunk[i:i + 16]
        text = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in row)
        print(f"{offset + i:08x}  {row.hex(' '):47}  {text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
