#!/usr/bin/env python3
"""Measure what a native port would have to replace: code size and hardware use.

Counts aligned words holding hardware addresses. That finds constants loaded from literal
pools, so it is a lower bound on register use and may include a few false hits in data.
Also lists wireless-related strings, sound archive contents and per-area code sizes.

Usage: python tools/survey_port.py
"""
import collections
import csv
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_image import all_code_images  # noqa: E402

GROUPS = [
    ("2D engine A display", 0x04000000, 0x04000070),
    ("DMA", 0x040000B0, 0x040000E0),
    ("timers", 0x04000100, 0x04000110),
    ("keys", 0x04000130, 0x04000134),
    ("IPC to ARM7", 0x04000180, 0x04000190),
    ("card", 0x040001A0, 0x040001C0),
    ("interrupts", 0x04000208, 0x04000218),
    ("VRAM banks", 0x04000240, 0x0400024A),
    ("math accelerator", 0x04000280, 0x040002C0),
    ("power and 3D control", 0x04000304, 0x04000340),
    ("3D registers", 0x04000340, 0x04000700),
    ("2D engine B display", 0x04001000, 0x04001070),
    ("IPC receive", 0x04100000, 0x04100014),
    ("palette RAM", 0x05000000, 0x05000800),
    ("BG VRAM A", 0x06000000, 0x06080000),
    ("BG VRAM B", 0x06200000, 0x06220000),
    ("OBJ VRAM A", 0x06400000, 0x06440000),
    ("OBJ VRAM B", 0x06600000, 0x06620000),
    ("LCDC VRAM", 0x06800000, 0x068A4000),
    ("OAM", 0x07000000, 0x07000800),
    ("shared work area", 0x027FF000, 0x02800000),
]
NEEDLES = re.compile(rb"[ -~]*(?:wireless|WM_|WMi|MB_|MBi|mbgui|download|dl_play|Download|Wi-?Fi|WFC|DWC|RTC|mic\b|multi)[ -~]*",
                     re.IGNORECASE)


def group_of(value):
    for name, low, high in GROUPS:
        if low <= value < high:
            return name
    return None


def sound_archive(path):
    with open(path, "rb") as f:
        data = f.read()
    info_off = struct.unpack_from("<I", data, 0x18)[0]
    names = ["SSEQ", "SSAR", "SBNK", "SWAR", "PLAYER", "GROUP", "PLAYER2", "STRM"]
    counts = {}
    for i, name in enumerate(names):
        rec = info_off + struct.unpack_from("<I", data, info_off + 8 + i * 4)[0]
        counts[name] = struct.unpack_from("<I", data, rec)[0]
    return len(data), counts


def main():
    totals = collections.Counter()
    images_using = collections.defaultdict(set)
    area_bytes = collections.Counter()
    hits = collections.Counter()
    for tag, base, data in all_code_images():
        area = "main" if tag in ("arm9", "itcm", "dtcm") else "overlays"
        area_bytes[area] += len(data)
        words = len(data) // 4
        for value in struct.unpack_from(f"<{words}I", data):
            name = group_of(value)
            if name:
                totals[name] += 1
                images_using[name].add(tag)
        for match in NEEDLES.finditer(data):
            text = match.group().decode("ascii", "replace").strip()
            if 4 <= len(text) <= 60:
                hits[(tag, text)] += 1

    print("code and data in images:", dict(area_bytes))
    print("\nhardware addresses held as words (group: words, images):")
    for name, _low, _high in GROUPS:
        print(f"  {name:24}{totals[name]:6}{len(images_using[name]):6}")

    print("\nimages touching 3D registers directly:",
          sorted(images_using["3D registers"])[:12], len(images_using["3D registers"]))
    print("\nwireless-related strings:")
    seen = 0
    for (tag, text), count in sorted(hits.items()):
        if seen < 40:
            print(f"  {tag:6} {text}")
        seen += 1
    print(f"  total {seen}")

    with open(os.path.join("unpacked", "arm7.bin"), "rb") as f:
        arm7 = f.read()
    print("\nARM7 size:", hex(len(arm7)))
    arm7_hits = {m.group().decode("ascii", "replace").strip() for m in NEEDLES.finditer(arm7)}
    print("ARM7 strings:", sorted(t for t in arm7_hits if 4 <= len(t) <= 60)[:20])

    size, counts = sound_archive(os.path.join("unpacked", "data", "MPDS_sound.sdat"))
    print("\nsound archive:", size, counts)

    with open("overlay_trace.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    print("\noverlay trace: instructions", sum(int(r["arm"]) + int(r["thumb"]) for r in rows),
          "unresolved indirect branches", sum(int(r["unresolved"]) for r in rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
