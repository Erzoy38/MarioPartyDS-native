#!/usr/bin/env python3
"""Dump the minigame table candidate: 24-byte records holding a title message id.

Usage: python tools/dump_mg_table.py [count]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_mpd import read_messages  # noqa: E402
from nds_image import Space  # noqa: E402

LAST_RECORD_TITLE = 0x020A28A8
TITLE_OFFSET_GUESS = None
STRIDE = 0x18


def main():
    space = Space()
    titles = read_messages("unpacked/data/UK/mpd_092_mgtitle_LZ.bin")
    # Walk back from the known field to find where the sequence of title ids starts.
    addr = LAST_RECORD_TITLE
    index = 72
    while True:
        prev = space.u32(addr - STRIDE)
        if prev != 0x00120000 + index - 1:
            break
        addr -= STRIDE
        index -= 1
    print(f"title field runs from {addr:#010x} (title {index}) to {LAST_RECORD_TITLE:#010x}")

    field = (addr - 0x020A0000) % STRIDE
    # Show raw words of records around the first title, then all records.
    print(f"title field offset modulo stride: {field:#x}")
    for rec in range(-2, 75 - index):
        base = addr + rec * STRIDE
        words = [space.u32(base + i * 4) for i in range(-4, 2)]
        text = " ".join(f"{w:08x}" if w is not None else "--------" for w in words)
        title = ""
        value = space.u32(base)
        if value is not None and value >> 16 == 0x12 and (value & 0xFFFF) < len(titles):
            title = titles[value & 0xFFFF]
        print(f"{base - 16:#010x}  {text}  {title}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
