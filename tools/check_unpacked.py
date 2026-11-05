#!/usr/bin/env python3
"""Sanity checks on the output of tools/decompress.py.

Usage: python tools/check_unpacked.py [unpacked_dir]
"""
import collections
import csv
import os
import struct
import sys


def signature(data):
    head = data[:4]
    if len(head) == 4 and all(0x20 <= b < 0x7F for b in head):
        return head.decode("ascii")
    return "(binary)"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "unpacked"

    print("overlay static-init entries:")
    with open("nds_overlays.csv", newline="") as f:
        overlays = list(csv.DictReader(f))
    where = collections.Counter()
    samples = []
    for ov in overlays:
        ov_id = int(ov["id"])
        ram = int(ov["ram"], 16)
        ram_size = int(ov["ram_size"], 16)
        bss = int(ov["bss_size"], 16)
        s0, s1 = int(ov["sinit_start"], 16), int(ov["sinit_end"], 16)
        with open(os.path.join(root, "overlay", f"ov{ov_id:03}.bin"), "rb") as f:
            image = f.read()
        for addr in range(s0, s1, 4):
            offset = addr - ram
            if not 0 <= offset <= len(image) - 4:
                where["table outside image"] += 1
                continue
            ptr = struct.unpack_from("<I", image, offset)[0]
            base = ptr & ~1
            if ram <= base < ram + ram_size:
                where["in overlay file data"] += 1
            elif ram + ram_size <= base < ram + ram_size + bss:
                where["in overlay bss"] += 1
            elif ptr == 0:
                where["null"] += 1
            elif 0x02000000 <= base < 0x021638A0:
                where["in main program"] += 1
            else:
                where["elsewhere"] += 1
            if len(samples) < 10:
                samples.append((ov_id, ram, ram_size, s0, s1, ptr))
    print("  " + ", ".join(f"{k}: {v}" for k, v in where.most_common()))
    for ov_id, ram, ram_size, s0, s1, ptr in samples:
        print(f"  ov{ov_id:03} ram={ram:#x}+{ram_size:#x} sinit={s0:#x}-{s1:#x} -> {ptr:#x}")

    print("\nARM code check, overlay instructions with the always condition (0xE):")
    ratios = []
    for name in sorted(os.listdir(os.path.join(root, "overlay"))):
        with open(os.path.join(root, "overlay", name), "rb") as f:
            image = f.read()
        words = len(image) // 4
        if words < 64:
            continue
        always = sum(1 for i in range(words) if image[i * 4 + 3] >> 4 == 0xE)
        ratios.append(always / words)
    ratios.sort()
    print(f"  overlays={len(ratios)} min={ratios[0]:.2f} "
          f"median={ratios[len(ratios) // 2]:.2f} max={ratios[-1]:.2f}")

    print("\nsignatures of unpacked data files (top 25):")
    sigs = collections.Counter()
    sizes = collections.Counter()
    for folder, _, names in os.walk(os.path.join(root, "data")):
        for name in names:
            with open(os.path.join(folder, name), "rb") as f:
                data = f.read()
            sig = signature(data)
            sigs[sig] += 1
            sizes[sig] += len(data)
    for sig, count in sigs.most_common(25):
        print(f"  {sig:10}{count:6}{sizes[sig]:12}")

    print("\ncontainers by member first bytes (from the raw report paths):")
    tags = collections.defaultdict(list)
    for folder, _, names in os.walk(os.path.join(root, "data")):
        if not folder.endswith(".d"):
            continue
        for name in sorted(names):
            with open(os.path.join(folder, name), "rb") as f:
                head = f.read(16)
            if name.endswith(".bin") and head[:1] in (b"\x20", b"\x54"):
                tags[head[0]].append((os.path.relpath(folder, root), name, head.hex(" ")))
    for tag, items in tags.items():
        print(f"  {tag:#04x}: {len(items)} members")
        for item in items[:4]:
            print(f"    {item[0]}/{item[1]}: {item[2]}")

    print("\nfile counts:")
    total = sum(len(names) for _, _, names in os.walk(root))
    size = sum(os.path.getsize(os.path.join(folder, n))
               for folder, _, names in os.walk(root) for n in names)
    print(f"  {total} files, {size} bytes in {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
