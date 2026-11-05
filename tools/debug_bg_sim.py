#!/usr/bin/env python3
"""Show what check_bg_calls.simulate sees for one function.

Usage: python tools/debug_bg_sim.py <overlay> <addr inside function>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_bg_calls import function_starts, simulate  # noqa: E402
from instruction_set import decode_arm  # noqa: E402
from nds_image import Space  # noqa: E402


def main():
    ov = int(sys.argv[1])
    addr = int(sys.argv[2], 16)
    space = Space(ov=ov)
    seg = next(s for s in space.segments if s.name == f"ov{ov:03}")
    data, base = seg.data, seg.base
    starts = function_starts(data)
    index = (addr - base) // 4
    start = max(s for s in starts if s <= index)
    end = min((s for s in starts if s > index), default=len(data) // 4)
    print(f"function {base + start * 4:#x}-{base + end * 4:#x} ({end - start} words)")
    print("first instructions:")
    for pos in range(start, min(start + 16, end)):
        word = struct.unpack_from("<I", data, pos * 4)[0]
        print(f"  {base + pos * 4:#x} {decode_arm(word, base + pos * 4).text}")
    for event in simulate(data, base, start, end):
        print("event:", event)
    return 0


if __name__ == "__main__":
    sys.exit(main())
