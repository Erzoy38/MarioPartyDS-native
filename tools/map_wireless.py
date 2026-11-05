#!/usr/bin/env python3
"""Finds the functions of the game's wireless helper (WH), multiboot (MB) and file system (WFS).

The helper library prints its own function names in error messages. For each such string this
finds the code that loads its address and the start of the enclosing function (the nearest
earlier push with lr). That gives name candidates, to be checked by reading the code.

Usage: python tools/map_wireless.py
"""
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import all_code_images  # noqa: E402

NAME = re.compile(rb"(WH_|WM_|WMi_|WFS_|WFSi_|MB_|MBi_|WBT_)[A-Za-z0-9_]+")


def main():
    images = list(all_code_images())
    for tag, base, data in images:
        strings = {}
        for match in re.finditer(rb"[ -~]{4,120}\x00", data):
            text = match.group()[:-1]
            name = NAME.search(text)
            if name:
                strings[base + match.start()] = (name.group().decode(), text.decode("ascii", "replace"))
        if not strings:
            continue
        words = struct.unpack_from(f"<{len(data) // 4}I", data)
        functions = collections.defaultdict(set)
        for i, word in enumerate(words):
            if word not in strings:
                continue
            literal_at = base + i * 4
            # The load is an "ldr rX,[pc,#n]" up to 4 KiB before its literal.
            for back in range(1, 1024):
                j = i - back
                if j < 0:
                    break
                insn = words[j]
                if (insn & 0x0F7F0000) == 0x051F0000 and (j * 4 + 8 + (insn & 0xFFF)) == i * 4:
                    start = j
                    while start > 0 and (words[start] & 0xFFFF4000) != 0xE92D4000:
                        start -= 1
                    functions[base + start * 4].add(strings[word][0])
                    break
        print(f"--- {tag}: {len(strings)} named strings, {len(functions)} functions")
        for start, names in sorted(functions.items()):
            print(f"  {start:#010x}  {', '.join(sorted(names))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
