#!/usr/bin/env python3
"""List direct ARM calls to a function and the constant passed in a register.

Looks back through the instructions before each call for the last write to the
register. Static and linear, so a branch between them can make a result wrong.

Usage: python tools/find_arg_calls.py <addr> [--reg N] [--back N]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import REG, decode_arm  # noqa: E402
from nds_image import all_code_images, parse_addr  # noqa: E402


def find_arg(data, base, index, reg, back):
    """Return a description of the value of reg before the word at index."""
    for step in range(1, back + 1):
        pos = index - step
        if pos < 0:
            break
        insn = decode_arm(struct.unpack_from("<I", data, pos * 4)[0], base + pos * 4)
        if insn.undefined or insn.ret or (insn.ends_flow and not insn.cond):
            return "dynamic (function start or flow break)"
        if insn.call:
            if reg <= 3:
                return "dynamic (earlier call)"
            continue
        if insn.rd != reg:
            continue
        if insn.mnem == "mov" and insn.ops.split(",")[1].startswith("#"):
            return int(insn.ops.split(",")[1][1:], 0)
        if insn.literal is not None and insn.mnem == "ldr":
            offset = insn.literal - base
            if 0 <= offset <= len(data) - 4:
                return struct.unpack_from("<I", data, offset)[0]
        return f"dynamic ({insn.text}) @{insn.addr:#x}"
    return "dynamic (not found)"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr")
    ap.add_argument("--reg", type=int, default=1)
    ap.add_argument("--back", type=int, default=16)
    args = ap.parse_args()
    target = parse_addr(args.addr)

    total = 0
    for tag, base, data in all_code_images():
        count = len(data) // 4
        for i, word in enumerate(struct.unpack_from(f"<{count}I", data)):
            if (word >> 24) & 0x0F != 0x0B or word >> 28 == 0xF:
                continue
            insn = decode_arm(word, base + i * 4)
            if insn.target != target:
                continue
            total += 1
            value = find_arg(data, base, i, args.reg, args.back)
            shown = f"{value} ({value:#x})" if isinstance(value, int) else value
            print(f"{tag:6} {base + i * 4:#010x}  {REG[args.reg]} = {shown}")
    print(f"{total} call(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
