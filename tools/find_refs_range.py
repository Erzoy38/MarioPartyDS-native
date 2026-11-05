#!/usr/bin/env python3
"""List aligned words in the code images whose value falls in an address range.

Usage: python tools/find_refs_range.py <start> <end> [--min-gap N] [--main-only]

Useful for finding what refers into a table, and for spotting the start of a
table: --min-gap prints only hits at least N bytes after the previous hit.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import all_code_images, parse_addr  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("start")
    ap.add_argument("end")
    ap.add_argument("--min-gap", type=int, default=0)
    ap.add_argument("--main-only", action="store_true")
    args = ap.parse_args()
    lo, hi = parse_addr(args.start), parse_addr(args.end)

    total = 0
    for tag, base, data in all_code_images():
        if args.main_only and tag.startswith("ov"):
            continue
        last = None
        count = len(data) // 4
        for i, word in enumerate(struct.unpack_from(f"<{count}I", data)):
            if lo <= word < hi:
                addr = base + i * 4
                total += 1
                if last is None or addr - last >= args.min_gap:
                    print(f"{tag:6} {addr:#010x}  -> {word:#010x}")
                last = addr
    print(f"{total} word(s) in range")
    return 0


if __name__ == "__main__":
    sys.exit(main())
