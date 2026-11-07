"""Compares the main RAM of network players dumped at the same exchange step.

Usage: python tools/compare_net_dumps.py <dump folder> [<dump folder>...]

Each folder is a `--dump-step N` dump (main_ram.bin). The first one is the reference. Prints how many
bytes differ and the differing ranges (merged when closer than 16 bytes), so that per-player data
(own aid, screens, stacks) can be told apart from the shared party state.
"""
import sys
from pathlib import Path

MAIN_RAM = 0x02000000


def ranges(a, b):
    out = []
    start = None
    last = None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
            elif i - last > 16:
                out.append((start, last))
                start = i
            last = i
    if start is not None:
        out.append((start, last))
    return out


def main():
    folders = [Path(p) for p in sys.argv[1:]]
    if len(folders) < 2:
        print(__doc__)
        return 1
    ref = (folders[0] / "main_ram.bin").read_bytes()
    worst = 0
    for folder in folders[1:]:
        other = (folder / "main_ram.bin").read_bytes()
        found = ranges(ref, other)
        count = sum(1 for x, y in zip(ref, other) if x != y)
        worst = max(worst, count)
        print(f"{folder}: {count} bytes differ in {len(found)} ranges")
        for start, end in found[:40]:
            print(f"  {MAIN_RAM + start:08x}-{MAIN_RAM + end:08x} ({end - start + 1} bytes)")
        if len(found) > 40:
            print(f"  ... {len(found) - 40} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
