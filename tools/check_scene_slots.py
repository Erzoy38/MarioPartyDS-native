#!/usr/bin/env python3
"""Check scenes.csv against the slot rule of 0x0201CF44 (scene >= 78 uses slot 1).

Usage: python tools/check_scene_slots.py
"""
import collections
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import load_overlay_table  # noqa: E402

SLOT_RAM = {0: 0x021638A0, 1: 0x021838A0}


def main():
    table = load_overlay_table()
    with open("scenes.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    wrong = []
    by_overlay = collections.defaultdict(list)
    for row in rows:
        scene, ov = int(row["scene"]), int(row["overlay"])
        by_overlay[ov].append(scene)
        expected = SLOT_RAM[1 if scene >= 78 else 0]
        if int(table[ov]["ram"], 16) != expected:
            wrong.append((scene, ov, table[ov]["ram"]))
    print(f"scenes={len(rows)} slot rule violations={len(wrong)} {wrong[:10]}")
    shared = {ov: s for ov, s in by_overlay.items() if len(s) > 1}
    print(f"overlays used by several scenes: {shared}")
    ordered = all(int(r["overlay"]) >= 0 for r in rows)
    print(f"all overlay ids valid: {ordered}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
