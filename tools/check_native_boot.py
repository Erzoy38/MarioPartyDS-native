#!/usr/bin/env python3
"""Compare memory dumped by the native run with the images already checked against melonDS.

The native run starts from the compressed ROM image, so a match means the translated game
code unpacked itself correctly. Usage: python tools/check_native_boot.py <dump folder>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import Space  # noqa: E402


def compare(name, live, expected, base):
    if len(live) < len(expected):
        print(f"{name}: dump too short")
        return False
    same = sum(1 for a, b in zip(live, expected) if a == b)
    print(f"{name:8} {same}/{len(expected)} bytes equal")
    if same == len(expected):
        return True
    shown = 0
    i = 0
    while i < len(expected) and shown < 10:
        if live[i] != expected[i]:
            print(f"  first differing at {base + i:#010x}: native {live[i:i + 8].hex(' ')} "
                  f"expected {expected[i:i + 8].hex(' ')}")
            shown += 1
            i += 64
        else:
            i += 1
    return False


def main():
    folder = sys.argv[1]
    with open(os.path.join(folder, "main_ram.bin"), "rb") as f:
        main_ram = f.read()
    with open(os.path.join(folder, "itcm.bin"), "rb") as f:
        itcm = f.read()
    with open(os.path.join(folder, "dtcm.bin"), "rb") as f:
        dtcm = f.read()

    space = Space()
    results = []
    for seg in space.segments:
        if seg.name == "arm9":
            results.append(compare("arm9", main_ram[:len(seg.data)], seg.data, seg.base))
        elif seg.name == "itcm":
            results.append(compare("itcm", itcm[:len(seg.data)], seg.data, seg.base))
        elif seg.name == "dtcm":
            results.append(compare("dtcm", dtcm[:len(seg.data)], seg.data, seg.base))
    print("all equal" if all(results) else "DIFFERENCES FOUND")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
