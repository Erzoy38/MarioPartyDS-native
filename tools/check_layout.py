#!/usr/bin/env python3
"""Ph1/Ph2 layout checks. Run tools/nds_header.py first (reads its CSV output).

Usage: python tools/check_layout.py <rom.nds>
"""
import collections
import csv
import struct
import sys


def main():
    with open(sys.argv[1], "rb") as f:
        rom = f.read()
    with open("nds_files.csv", newline="") as f:
        files = list(csv.DictReader(f))

    overlays = [r for r in files if r["path"].startswith("overlay")]
    named = [r for r in files if r["path"].startswith("data/")]
    for label, rows in (("overlays", overlays), ("data files", named)):
        start = min(int(r["rom_off"], 16) for r in rows)
        end = max(int(r["rom_off"], 16) + int(r["size"]) for r in rows)
        print(f"{label:12} rom range {start:#x}-{end:#x}")

    used = struct.unpack_from("<I", rom, 0x80)[0]
    tail = rom[used:]
    print(f"tail         {used:#x}-{len(rom):#x} bytes={len(tail):#x} "
          f"distinct_values={sorted(set(tail))[:4]}")

    arm9_off, arm9_size = struct.unpack_from("<II", rom, 0x20)[0], 0
    arm9_size = struct.unpack_from("<I", rom, 0x2C)[0]
    end = arm9_off + arm9_size
    # BLZ footer: u24 compressed length, u8 footer length, u32 size increase
    word0, inc = struct.unpack_from("<II", rom, end - 8)
    comp_len, footer_len = word0 & 0xFFFFFF, word0 >> 24
    print(f"arm9 blz     comp_len={comp_len:#x} footer_len={footer_len:#x} "
          f"inc={inc:#x} decompressed_size={arm9_size + inc:#x}")

    print("\n*_LZ named files, by first byte:")
    lz_named = [r for r in named if "_lz" in r["path"].lower()]
    first = collections.Counter()
    plausible = 0
    for r in lz_named:
        off, size = int(r["rom_off"], 16), int(r["size"])
        head = rom[off:off + 4]
        first[head[0]] += 1
        # LZ10: tag 0x10 then a u24 decompressed size. Must expand, within reason.
        out = int.from_bytes(head[1:4], "little")
        if head[0] == 0x10 and size <= out * 9 // 8 + 16 and out <= size * 64:
            plausible += 1
    print(f"  count={len(lz_named)} plausible_lz10_header={plausible}")
    for byte, count in first.most_common(8):
        print(f"  {byte:#04x}: {count}")

    print("\n*_LZ named files without an LZ10 tag (first 6):")
    shown = 0
    for r in lz_named:
        off, size = int(r["rom_off"], 16), int(r["size"])
        if rom[off] != 0x10 and shown < 6:
            print(f"  {r['path']:36}{size:8}  {rom[off:off + 16].hex(' ')}")
            shown += 1

    print("\nnon-_LZ files with an LZ10-looking tag:")
    other = [r for r in named if "_lz" not in r["path"].lower()
             and rom[int(r["rom_off"], 16)] == 0x10]
    print(f"  count={len(other)}  e.g. {[r['path'] for r in other[:4]]}")

    print("\nlanguage folders: files only in FR vs UK:")
    def names(lang):
        prefix = f"data/{lang}/"
        return {r["path"][len(prefix):] for r in named if r["path"].startswith(prefix)}
    print(f"  FR-UK={sorted(names('FR') - names('UK'))}")
    print(f"  UK-FR={sorted(names('UK') - names('FR'))}")

    print("\nname stems of data/ root files (top 15):")
    stems = collections.Counter(r["path"].split("/")[1].split("_")[0]
                                for r in named if r["path"].count("/") == 1)
    print("  " + ", ".join(f"{s}:{c}" for s, c in stems.most_common(15)))
    print("\nname stems of data/UK files (top 15):")
    stems = collections.Counter(r["path"].split("/")[2].split("_")[0]
                                for r in named if r["path"].startswith("data/UK/"))
    print("  " + ", ".join(f"{s}:{c}" for s, c in stems.most_common(15)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
