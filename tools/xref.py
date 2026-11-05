#!/usr/bin/env python3
"""Find references to an address in the ARM9 program and all overlays.

Usage: python tools/xref.py <addr> [--ov N] [--no-data]

The binary does not record which words are code, so this scans every aligned
position in both instruction sets. Treat matches as candidates:
  data       a word holding the address (literal pool or pointer table)
  arm-*      ARM branch or call
  thumb-*    Thumb branch or call

For a target in the overlay area, --ov N limits direct branches to callers that
can reach it: overlay N itself plus the main program and overlays in other slots.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import sext  # noqa: E402
from nds_image import all_code_images, load_overlay_table, parse_addr  # noqa: E402


def scan(data, base, target, want_data):
    target_even = target & ~1
    hits = []
    words = len(data) // 4
    for i, word in enumerate(struct.unpack_from(f"<{words}I", data)):
        addr = base + i * 4
        if want_data and word in (target, target | 1, target_even):
            hits.append((addr, "data", f"{word:#010x}"))
        kind = word >> 24 & 0x0F
        if kind in (0x0A, 0x0B) and word >> 28 != 0xF:
            dest = (addr + 8 + (sext(word & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
            if dest == target_even:
                name = "arm-bl" if kind == 0x0B else "arm-b"
                hits.append((addr, name, f"{word:08x}"))
        elif word >> 25 == 0x7D:
            dest = (addr + 8 + (sext(word & 0xFFFFFF, 24) << 2) + ((word >> 23) & 2)) & 0xFFFFFFFF
            if dest == target_even:
                hits.append((addr, "arm-blx", f"{word:08x}"))

    halves = len(data) // 2
    h = struct.unpack_from(f"<{halves}H", data)
    for i, first in enumerate(h):
        addr = base + i * 2
        top = first >> 11
        pc = addr + 4
        if top == 0b11110 and i + 1 < halves and h[i + 1] >> 11 in (0b11111, 0b11101):
            second = h[i + 1]
            dest = pc + (sext(first & 0x7FF, 11) << 12) + ((second & 0x7FF) << 1)
            is_blx = second >> 11 == 0b11101
            if is_blx:
                dest &= ~3
            if dest & 0xFFFFFFFF == target_even:
                hits.append((addr, "thumb-blx" if is_blx else "thumb-bl",
                             f"{first:04x}{second:04x}"))
        elif top == 0b11100:
            if (pc + (sext(first & 0x7FF, 11) << 1)) & 0xFFFFFFFF == target_even:
                hits.append((addr, "thumb-b", f"{first:04x}"))
        elif first >> 12 == 0b1101 and (first >> 8) & 15 < 14:
            if (pc + (sext(first & 0xFF, 8) << 1)) & 0xFFFFFFFF == target_even:
                hits.append((addr, "thumb-bcc", f"{first:04x}"))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr")
    ap.add_argument("--ov", type=int)
    ap.add_argument("--no-data", action="store_true")
    args = ap.parse_args()
    target = parse_addr(args.addr)

    same_slot = None
    if args.ov is not None:
        table = load_overlay_table()
        slot = table[args.ov]["ram"]
        same_slot = {f"ov{i:03}" for i, r in table.items() if r["ram"] == slot and i != args.ov}

    total = 0
    for tag, base, data in all_code_images():
        if same_slot and tag in same_slot:
            continue
        for addr, kind, raw in scan(data, base, target, not args.no_data):
            print(f"{tag:6} {addr:#010x}  {kind:10} {raw}")
            total += 1
    print(f"{total} candidate(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
