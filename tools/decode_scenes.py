#!/usr/bin/env python3
"""Decode the scene tables and check them against the overlays, then trace each.

  0x02099EE8  u32 overlay id per scene
  0x0209A0AC  three u32 function pointers per scene (phases 0, 1, 2)

Every pointer must land inside the overlay named by the first table and decode as
a plausible function start. Writes scenes.csv and overlay_trace.csv.

Usage: python tools/decode_scenes.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import decode_at  # noqa: E402
from instruction_set import decode_arm  # noqa: E402
from nds_image import Segment, Space, load_overlay_table  # noqa: E402
from trace_code import Tracer  # noqa: E402

SCENE_OVERLAY = 0x02099EE8
SCENE_FUNCS = 0x0209A0AC


def main():
    table = load_overlay_table()
    main_space = Space()
    count = (SCENE_FUNCS - SCENE_OVERLAY) // 4
    print(f"scenes from table spacing: {count}")

    rows = []
    checks = collections.Counter()
    first_ops = collections.Counter()
    problems = []
    for scene in range(count):
        ov = main_space.u32(SCENE_OVERLAY + scene * 4)
        funcs = [main_space.u32(SCENE_FUNCS + scene * 12 + i * 4) for i in range(3)]
        if ov not in table:
            checks["overlay id not in table"] += 1
            problems.append(f"scene {scene}: overlay id {ov}")
            rows.append([scene, ov, *[f"{f:#010x}" for f in funcs], "bad id"])
            continue
        row = table[ov]
        lo = int(row["ram"], 16)
        hi = lo + int(row["ram_size"], 16)
        space = Space(ov=ov)
        status = []
        for i, func in enumerate(funcs):
            if func == 0:
                checks["null pointer"] += 1
                continue
            if not lo <= func & ~1 < hi:
                checks["pointer outside its overlay"] += 1
                status.append(f"p{i} outside")
                problems.append(f"scene {scene} ov{ov} p{i}: {func:#010x} not in {lo:#x}-{hi:#x}")
                continue
            checks["pointer inside its overlay"] += 1
            insn = decode_at(space, func & ~1, bool(func & 1))
            first_ops[insn.mnem] += 1
            if insn.undefined:
                checks["pointer to undefined instruction"] += 1
                status.append(f"p{i} undefined")
        rows.append([scene, ov, *[f"{f:#010x}" for f in funcs], " ".join(status) or "ok"])

    used = {r[1] for r in rows}
    print("checks: " + ", ".join(f"{k}={v}" for k, v in checks.most_common()))
    print("first mnemonics: " + ", ".join(f"{k}:{v}" for k, v in first_ops.most_common(10)))
    for line in problems[:20]:
        print("  " + line)
    missing = sorted(set(table) - used)
    print(f"overlays referenced: {len(used & set(table))} of {len(table)}; not referenced: {missing}")
    print(f"scenes per slot: {collections.Counter(table[r[1]]['ram'] for r in rows if r[1] in table)}")
    print(f"scenes with overlay 0: {[r[0] for r in rows if r[1] == 0]}")

    with open("scenes.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scene", "overlay", "phase0", "phase1", "phase2", "status"])
        w.writerows(rows)

    # Trace every overlay from its scene pointers, staying inside the overlay.
    entries = collections.defaultdict(set)
    for row in rows:
        for text in row[2:5]:
            value = int(text, 16)
            if value and row[1] in table:
                entries[row[1]].add(value)

    # Add the per-mode entry functions from 0x02099BFC (see decode_modes.py).
    for mode in range(0x2C, 0x9D):
        scene = struct.unpack("<h", main_space.read(0x02099B18 + (mode - 0x2C) * 2, 2))[0]
        value = main_space.u32(0x02099BFC + mode * 4)
        ov = main_space.u32(SCENE_OVERLAY + scene * 4)
        if value:
            entries[ov].add(value)

    arm = thumb = covered = total = undefined = conflicts = 0
    out = []
    outside = collections.Counter()
    for ov, seeds in sorted(entries.items()):
        space = Space(ov=ov)
        seg = next(s for s in space.segments if isinstance(s, Segment) and s.name == f"ov{ov:03}")
        tracer = Tracer(space, [seg])
        for value in sorted(seeds):
            tracer.add(value & ~1, bool(value & 1), "scene")
        tracer.run()
        a, t, c = tracer.stats()
        arm += a
        thumb += t
        covered += c
        total += len(seg.data)
        undefined += len(tracer.undefined)
        conflicts += len(tracer.conflicts)
        outside.update(tracer.outside)
        out.append([ov, len(seeds), len(tracer.functions), a, t, hex(c), hex(len(seg.data)),
                    f"{100 * c / max(len(seg.data), 1):.1f}", len(tracer.undefined),
                    tracer.resolved, tracer.unresolved])
        for addr, is_thumb, raw, _ in tracer.undefined[:2]:
            print(f"  undefined ov{ov:03} {addr:#010x} {'thumb' if is_thumb else 'arm'} {raw:#x}")
    print(f"overlay trace: overlays={len(out)} arm={arm} thumb={thumb} undefined={undefined} "
          f"conflicts={conflicts} bytes={covered:#x}/{total:#x} ({100 * covered / total:.1f}%)")
    print("  leaving overlay: " + ", ".join(f"{k}={v}" for k, v in outside.most_common()))
    with open("overlay_trace.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["overlay", "seeds", "functions", "arm", "thumb", "bytes", "size",
                    "percent", "undefined", "resolved", "unresolved"])
        w.writerows(out)
    ratios = sorted(float(r[7]) for r in out if int(r[6], 16) > 0x100)
    if ratios:
        print(f"  coverage per overlay: min={ratios[0]} median={ratios[len(ratios) // 2]} "
              f"max={ratios[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
