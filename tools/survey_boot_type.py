#!/usr/bin/env python3
"""Where does the game act differently when started by Download Play?

0x027FFC40 holds the boot type; start_overlay treats 2 as a download start. This lists every
word holding that address, and what the checksum table at 0x020BD408 implies.

Usage: python tools/survey_boot_type.py
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import decode_at  # noqa: E402
from nds_image import Space, all_code_images  # noqa: E402

BOOT_TYPE = 0x027FFC40
TABLE_START = 0x020BD408
TABLE_END = 0x020BDC78


def uses_of(space, literal_addr, base_start, base_end):
    """Instructions before a literal that load it, with what follows."""
    out = []
    for addr in range(max(base_start, literal_addr - 0x1000), literal_addr, 4):
        insn = decode_at(space, addr, False)
        if insn is not None and insn.literal == literal_addr:
            lines = []
            for i in range(0, 5):
                follow = decode_at(space, addr + i * 4, False)
                if follow is not None:
                    lines.append(follow.text)
            out.append((addr, " | ".join(lines)))
    return out


def main():
    print(f"checksum table: {(TABLE_END - TABLE_START) / 20} entries of 20 bytes")
    space = Space()
    print("table start, first two entries:",
          space.read(TABLE_START, 40).hex(" "))
    per_image = collections.Counter()
    for tag, base, data in all_code_images():
        words = len(data) // 4
        for i, value in enumerate(struct.unpack_from(f"<{words}I", data)):
            if value != BOOT_TYPE:
                continue
            per_image[tag] += 1
            literal = base + i * 4
            ov = int(tag[2:]) if tag.startswith("ov") else None
            image_space = Space(ov=ov) if ov is not None else space
            for addr, text in uses_of(image_space, literal, base, base + len(data)):
                print(f"{tag:6} {addr:#010x}  {text}")
    print("words by image:", dict(per_image))
    return 0


if __name__ == "__main__":
    sys.exit(main())
