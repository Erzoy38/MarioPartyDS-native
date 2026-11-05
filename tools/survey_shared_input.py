#!/usr/bin/env python3
"""Which modules use the four-player input path and the wireless state?

Counts direct calls to the wireless state functions and words pointing at the per-player
input structures, per overlay, then joins that with the mode and minigame lists.

Usage: python tools/survey_shared_input.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import sext  # noqa: E402
from nds_image import all_code_images  # noqa: E402

# Named after what the code shows: 0x02027110 is 0 when wireless is off, else WH state + 1.
# 0x020245A0 reads a halfword from the WH state; "local_id" is a guess at its meaning.
CALLS = {0x02027110: "wireless_players", 0x020245A0: "local_id", 0x020245B0: "wh_state"}
PLAYER_INPUT = (0x020CFCD8, 0x020CFCD8 + 4 * 0x38)
SINGLE_INPUT = (0x020CFBC0, 0x020CFCD8)


def main():
    usage = collections.defaultdict(collections.Counter)
    for tag, base, data in all_code_images():
        words = len(data) // 4
        for i, word in enumerate(struct.unpack_from(f"<{words}I", data)):
            if PLAYER_INPUT[0] <= word < PLAYER_INPUT[1]:
                usage[tag]["player_input"] += 1
            elif SINGLE_INPUT[0] <= word < SINGLE_INPUT[1]:
                usage[tag]["single_input"] += 1
            if (word >> 24) & 0x0F == 0x0B and word >> 28 != 0xF:
                dest = (base + i * 4 + 8 + (sext(word & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
                if dest in CALLS:
                    usage[tag][CALLS[dest]] += 1

    with open("minigames.csv", newline="", encoding="utf-8") as f:
        games = {int(r["overlay"]): r for r in csv.DictReader(f)}
    with open("overlay_files.csv", newline="") as f:
        files = {int(r["overlay"]): r for r in csv.DictReader(f)}

    keys = ["wireless_players", "local_id", "wh_state", "player_input", "single_input"]
    print("main program:", {t: dict(usage[t]) for t in ("arm9", "itcm", "dtcm")})
    rows = []
    for ov in range(108):
        tag = f"ov{ov:03}"
        rows.append((ov, usage[tag]))
    with_wireless = [o for o, u in rows if u["wireless_players"]]
    with_id = [o for o, u in rows if u["local_id"]]
    with_input = [o for o, u in rows if u["player_input"]]
    print(f"overlays asking the wireless state: {len(with_wireless)}")
    print(f"overlays asking the local player id: {len(with_id)}")
    print(f"overlays reading per-player input directly: {len(with_input)}")

    print("\nminigames (number, name, wireless calls, id calls, player input, single input):")
    groups = collections.Counter()
    missing = []
    for ov, game in sorted(games.items()):
        u = usage[f"ov{ov:03}"]
        aware = u["wireless_players"] or u["local_id"]
        group = game["mg_id"][:1]
        groups[(group, "aware" if aware else "not aware")] += 1
        if not aware:
            missing.append(f"{game['mg_id']} {game['UK']}")
        print(f"  {game['mg_id']:5} {game['UK'][:24]:24} "
              + " ".join(f"{u[k]:3}" for k in keys))
    print("\nby group:", dict(sorted(groups.items())))
    print("not aware:", missing)

    print("\nnon-minigame overlays asking the wireless state:")
    for ov in with_wireless:
        if ov not in games:
            print(f"  ov{ov:03} {files[ov]['modes']:10} {files[ov]['files'][:60]}")
    print("non-minigame overlays NOT asking it:")
    print("  " + ", ".join(f"ov{o:03}" for o, _u in rows if o not in games and o not in with_wireless))
    return 0


if __name__ == "__main__":
    sys.exit(main())
