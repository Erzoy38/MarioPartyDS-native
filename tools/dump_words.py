#!/usr/bin/env python3
"""Dump memory as signed 16-bit values, 8 per line.

Usage: python tools/dump_words.py <addr> <count> [--ov N]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import Space, parse_addr  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("addr")
    ap.add_argument("count", type=int)
    ap.add_argument("--ov", type=int)
    args = ap.parse_args()
    space = Space(ov=args.ov)
    addr = parse_addr(args.addr)
    for row in range(0, args.count, 8):
        values = []
        for i in range(row, min(row + 8, args.count)):
            raw = space.read(addr + i * 2, 2)
            values.append("   --" if raw is None else f"{struct.unpack('<h', raw)[0]:5}")
        print(f"{addr + row * 2:#010x} " + " ".join(values))
    return 0


if __name__ == "__main__":
    sys.exit(main())
