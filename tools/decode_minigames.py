#!/usr/bin/env python3
"""Decode the minigame table at 0x020A216C and join it with modes and overlays.

Record (24 bytes, indexed by scene, scenes 0-77):
  +0x00 flags, +0x04 title, +0x08 instructions, +0x0C and +0x10 controls, +0x14 tip
Text fields are message ids: (mpd file index << 16) | message.
Empty records hold 0 (removed minigames) or 0xFFFFFFFF (scene 0).

Writes minigames.csv and MINISPIELE.md. Usage: python tools/decode_minigames.py
"""
import collections
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_mpd import read_messages  # noqa: E402
from nds_image import Space  # noqa: E402

TABLE = 0x020A216C
STRIDE = 0x18
COUNT = 78
NONE = 0xFFFFFFFF
LANGS = ("UK", "GE", "FR", "IT", "SP")
FILES = {0x10: "mpd_090_mginst_LZ.bin", 0x11: "mpd_091_mgguid_LZ.bin",
         0x12: "mpd_092_mgtitle_LZ.bin", 0x15: "mpd_095_mgadv_LZ.bin"}
GROUPS = {"1": "Nummer 1xx", "2": "Nummer 2xx", "3": "Nummer 3xx",
          "4": "Nummer 4xx", "5": "Nummer 5xx (Bosse)", "6": "Nummer 6xx (Puzzle)"}


def load_messages(lang):
    return {index: read_messages(f"unpacked/data/{lang}/{name}") for index, name in FILES.items()}


def lookup(messages, msg_id):
    if msg_id in (0, NONE):
        return ""
    return messages[msg_id >> 16][msg_id & 0xFFFF]


def clean(text):
    return re.sub(r"\[/?[a-z]*\d*\]", "", text).replace("\n", " ").strip()


def main():
    space = Space()
    with open("modes.csv", newline="") as f:
        modes = {int(r["scene"]): r for r in csv.DictReader(f)}
    with open("overlay_files.csv", newline="") as f:
        files = {int(r["overlay"]): r for r in csv.DictReader(f)}
    text = {lang: load_messages(lang) for lang in LANGS}

    rows = []
    problems = []
    titles_seen = []
    empty_scenes = []
    for scene in range(COUNT):
        flags, title, inst, guide1, guide2, tip = (
            space.u32(TABLE + scene * STRIDE + i * 4) for i in range(6))
        mode = modes[scene]
        overlay = int(mode["overlay"])
        ids = files[overlay]["mg_ids"].replace("000b", "").split()
        mg = ids[-1] if ids else ""
        empty = title in (0, NONE)
        if empty != (overlay == 0):
            problems.append(f"scene {scene}: empty={empty} overlay={overlay}")
        if empty:
            empty_scenes.append(scene)
            continue
        for value, file_index in ((title, 0x12), (inst, 0x10), (guide1, 0x11),
                                  (guide2, 0x11), (tip, 0x15)):
            if value != NONE and value >> 16 != file_index:
                problems.append(f"scene {scene}: field {value:#x} not in file {file_index:#x}")
        titles_seen.append(title & 0xFFFF)
        names = [lookup(text[lang], title) for lang in LANGS]
        rows.append({
            "scene": scene, "mode": mode["mode"], "overlay": overlay, "mg_id": mg,
            "flags": flags, "title_index": title & 0xFFFF, "names": names,
            "inst": clean(lookup(text["UK"], inst)),
            "controls": clean(lookup(text["UK"], guide1)),
        })

    with open("minigames.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["scene", "mode", "overlay", "mg_id", "flags", "title_index", *LANGS,
                    "instructions_uk", "controls_uk"])
        for r in rows:
            w.writerow([r["scene"], r["mode"], r["overlay"], r["mg_id"], f"{r['flags']:#010x}",
                        r["title_index"], *r["names"], r["inst"], r["controls"]])

    with open("MINISPIELE.md", "w", encoding="utf-8") as f:
        f.write("# Minispiele in Mario Party DS\n\n")
        f.write("Erzeugt von `tools/decode_minigames.py` aus der Tabelle bei 0x020A216C. "
                "Die Namen stammen aus den Textdateien des Spiels.\n")
        for group, label in GROUPS.items():
            members = [r for r in rows if r["mg_id"].startswith(group)]
            f.write(f"\n## {label}: {len(members)} Spiele\n\n")
            f.write("| Nummer | Modus | Overlay | Englisch | Deutsch |\n|---|---|---|---|---|\n")
            for r in members:
                f.write(f"| {r['mg_id']} | {r['mode']} | {r['overlay']} | "
                        f"{r['names'][0]} | {r['names'][1]} |\n")

    print(f"records with a title: {len(titles_seen)} of {COUNT}; empty scenes: {empty_scenes}")
    print(f"title indices distinct: {len(set(titles_seen))}; "
          f"cover 0-72: {sorted(titles_seen) == list(range(73))}")
    moved = [(r["mg_id"], r["title_index"]) for i, r in enumerate(rows) if r["title_index"] != i]
    print(f"titles not at their position in table order: {len(moved)}; first {moved[:4]}")
    print(f"problems: {len(problems)}")
    for line in problems[:12]:
        print("  " + line)
    print(f"instructions missing: {[r['mg_id'] for r in rows if not r['inst']]}")
    print(f"mg id groups: {collections.Counter(r['mg_id'][:1] for r in rows)}")

    print("\nlow byte of flags by number group:")
    by_group = collections.defaultdict(collections.Counter)
    for r in rows:
        by_group[r["mg_id"][:1]][f"{r['flags'] & 0xFF:#04x}"] += 1
    for group, counter in sorted(by_group.items()):
        print(f"  {group}xx: {dict(counter)}")

    print("\ntop byte of flags vs. text:")
    tops = collections.defaultdict(list)
    for r in rows:
        tops[r["flags"] >> 24].append(r)
    for top, members in sorted(tops.items()):
        words = collections.Counter(
            re.sub(r"[^A-Za-z +]", " ", m["controls"]).strip().split("  ")[0][:22] for m in members)
        print(f"  {top:#04x} ({len(members)}): {words.most_common(4)}")

    mic_text = {r["mg_id"] for r in rows if "microphone" in r["inst"].lower()}
    mic_flag = {r["mg_id"] for r in rows if r["flags"] >> 24 == 8}
    print(f"\nmicrophone in UK instructions: {sorted(mic_text)}")
    print(f"top byte 0x08:                  {sorted(mic_flag)}")
    print(f"identical: {mic_text == mic_flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
