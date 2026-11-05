#!/usr/bin/env python3
"""Compare the running game's RAM with the unpacked images. Read-only; the game runs on.

Expected differences: variables in data sections. Differences in code would point to a
wrong unpacker, or to code patched at run time. Code ranges come from functions.csv.

Needs tools/gdb_server.py running. Usage: python tools/verify_ram.py
"""
import bisect
import csv
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402
from nds_image import Space, load_overlay_table  # noqa: E402

SLOT_SCENE = 0x020CEFB4
SCENE_OVERLAY = 0x02099EE8


def diff_ranges(live, expected, base, gap=16):
    """Merge differing bytes into ranges, joining those closer than gap."""
    ranges = []
    for i, (a, b) in enumerate(zip(live, expected)):
        if a != b:
            if ranges and i - ranges[-1][1] <= gap:
                ranges[-1][1] = i
            else:
                ranges.append([i, i])
    return [(base + s, base + e + 1) for s, e in ranges]


def compare(gdb, name, base, expected):
    start = time.time()
    live = gdb.read(base, len(expected))
    same = sum(1 for a, b in zip(live, expected) if a == b)
    ranges = diff_ranges(live, expected, base)
    print(f"{name:10} {base:#010x} size {len(expected):#8x}: {same}/{len(expected)} bytes equal "
          f"({100 * same / len(expected):.2f}%), {len(ranges)} differing ranges, "
          f"{time.time() - start:.1f}s")
    return ranges


def function_starts():
    with open("functions.csv", newline="") as f:
        return sorted(int(r["addr"], 16) for r in csv.DictReader(f))


def main():
    space = Space()
    starts = function_starts()
    table = load_overlay_table()

    with Session() as gdb:
        print("interrupt:", gdb.interrupt())
        regs, _ = gdb.registers()
        print(f"pc={regs['pc']:#010x} lr={regs['lr']:#010x} sp={regs['sp']:#010x}")

        for seg in space.segments:
            ranges = compare(gdb, seg.name, seg.base, seg.data)
            total = sum(e - s for s, e in ranges)
            print(f"  differing span in total: {total:#x} bytes")
            for s, e in ranges[:14]:
                print(f"    {s:#010x}-{e:#010x} ({e - s:#x})")
            if len(ranges) > 14:
                print(f"    ... {len(ranges) - 14} more; last ends {ranges[-1][1]:#010x}")
            if seg.name == "arm9" and starts:
                code_end = starts[-1]
                inside = [(s, e) for s, e in ranges if s < code_end]
                print(f"  ranges before the last traced function ({code_end:#x}): {len(inside)}")
                for s, e in inside[:10]:
                    i = bisect.bisect_right(starts, s) - 1
                    owner = starts[i] if i >= 0 else 0
                    print(f"    {s:#010x}-{e:#010x} after function {owner:#010x}: "
                          f"{gdb.read(s, min(e - s, 16)).hex(' ')}")

        for slot in range(2):
            scene = struct.unpack("<h", gdb.read(SLOT_SCENE + slot * 2, 2))[0]
            flag_note = "" if scene else " (0: scene 0 or nothing loaded)"
            print(f"\nslot {slot}: scene {scene}{flag_note}")
            if scene <= 0:
                continue
            ov = struct.unpack("<I", space.read(SCENE_OVERLAY + scene * 4, 4))[0]
            row = table[ov]
            with open(os.path.join("unpacked", "overlay", f"ov{ov:03}.bin"), "rb") as f:
                image = f.read()
            ranges = compare(gdb, f"ov{ov:03}", int(row["ram"], 16), image)
            for s, e in ranges[:12]:
                print(f"    {s:#010x}-{e:#010x} ({e - s:#x})")
            if len(ranges) > 12:
                print(f"    ... {len(ranges) - 12} more")
        mode = gdb.read(0x020C9660, 16)
        print(f"\nmode_state: scene={struct.unpack('<h', mode[:2])[0]} prev={mode[0xC]:#x} "
              f"next={mode[0xD]:#x} current={mode[0xF]:#x}")
    print("done; the game should keep running")
    return 0


if __name__ == "__main__":
    sys.exit(main())
