#!/usr/bin/env python3
"""Compare each mode's entry function with the scene's phase pointers.

Usage: python tools/check_mode_phase.py  (reads modes.csv and scenes.csv)
"""
import collections
import csv
import sys


def main():
    with open("modes.csv", newline="") as f:
        modes = list(csv.DictReader(f))
    with open("scenes.csv", newline="") as f:
        scenes = {int(r["scene"]): r for r in csv.DictReader(f)}
    tally = collections.Counter()
    other = []
    for mode in modes:
        scene = scenes[int(mode["scene"])]
        entry = int(mode["entry"], 16)
        matches = [p for p in ("phase0", "phase1", "phase2") if int(scene[p], 16) == entry]
        tally[",".join(matches) or "none"] += 1
        if not matches:
            other.append((mode["mode"], mode["overlay"], mode["entry"], scene["phase0"],
                          scene["phase1"], scene["phase2"]))
    print(dict(tally))
    for row in other[:12]:
        print("  no match:", row)
    nulls = collections.Counter()
    for scene in scenes.values():
        for phase in ("phase0", "phase1", "phase2"):
            if int(scene[phase], 16) == 0:
                nulls[phase] += 1
    print(f"null pointers per phase: {dict(nulls)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
