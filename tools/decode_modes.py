#!/usr/bin/env python3
"""Join game mode, scene and overlay, and check the per-mode entry functions.

  0x02099B18  s16 scene per mode, for modes >= 0x2C (read by 0x0201A3FC)
  0x02099BFC  u32 entry function per mode (read by 0x0201A448)
  0x02099EE8  u32 overlay per scene

0x02099BFC ends where the next table starts (0x02099E70), which gives the mode
count. Writes modes.csv. Usage: python tools/decode_modes.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import decode_at  # noqa: E402
from nds_image import Space, load_overlay_table  # noqa: E402

SCENE_BY_MODE = 0x02099B18
MODE_TABLE = 0x02099BFC
MODE_TABLE_END = 0x02099E70
SCENE_OVERLAY = 0x02099EE8
SCENE_COUNT = 113
FIRST_SCENE_MODE = 0x2C


def main():
    space = Space()
    table = load_overlay_table()
    modes = (MODE_TABLE_END - MODE_TABLE) // 4
    print(f"modes from table spacing: {modes} (0x0-{modes - 1:#x})")
    slots = (MODE_TABLE - SCENE_BY_MODE) // 2
    last = struct.unpack("<h", space.read(SCENE_BY_MODE + (slots - 1) * 2, 2))[0]
    print(f"scene table: {slots} s16 slots, needed {modes - FIRST_SCENE_MODE}, last slot value {last}")

    low = {m: space.u32(MODE_TABLE + m * 4) for m in range(FIRST_SCENE_MODE)}
    print("modes below 0x2C with an entry function: "
          + ", ".join(f"{m:#x}={v:#010x}" for m, v in low.items() if v))

    checks = collections.Counter()
    first_ops = collections.Counter()
    rows = []
    scenes = []
    problems = []
    for mode in range(FIRST_SCENE_MODE, modes):
        scene = struct.unpack("<h", space.read(SCENE_BY_MODE + (mode - FIRST_SCENE_MODE) * 2, 2))[0]
        scenes.append(scene)
        func = space.u32(MODE_TABLE + mode * 4)
        ov = space.u32(SCENE_OVERLAY + scene * 4)
        row = table[ov]
        lo, hi = int(row["ram"], 16), int(row["ram"], 16) + int(row["ram_size"], 16)
        ov_space = Space(ov=ov)
        if lo <= func < hi:
            checks["in its overlay"] += 1
        elif ov_space.segment_at(func) is not None:
            checks["in main program"] += 1
        else:
            checks["outside"] += 1
            problems.append(f"mode {mode:#x} scene {scene} ov{ov}: {func:#010x}")
        insn = decode_at(ov_space, func & ~1, bool(func & 1))
        if insn is not None:
            first_ops[insn.mnem] += 1
            if insn.undefined:
                checks["undefined at entry"] += 1
        rows.append([f"{mode:#x}", scene, ov, f"{func:#010x}"])

    print(f"entry functions: {dict(checks)}")
    print("first mnemonics: " + ", ".join(f"{k}:{v}" for k, v in first_ops.most_common(8)))
    for line in problems[:12]:
        print("  " + line)
    print(f"distinct scenes: {len(set(scenes))} of {SCENE_COUNT}; "
          f"repeats: { {s: c for s, c in collections.Counter(scenes).items() if c > 1} }")
    identity = all(s == i for i, s in enumerate(scenes))
    print(f"scene equals mode - 0x2C for every mode: {identity}")
    if not identity:
        diffs = [(hex(FIRST_SCENE_MODE + i), s) for i, s in enumerate(scenes) if s != i][:20]
        print(f"  first differences (mode, scene): {diffs}")

    with open("modes.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["mode", "scene", "overlay", "entry"])
        w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
