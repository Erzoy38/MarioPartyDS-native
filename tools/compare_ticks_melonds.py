#!/usr/bin/env python3
"""Run melonDS tick by tick from the start and compare it with the native run.

Stops melonDS at every call of game_tick, counts them, and notes when the scene changes. At the
checkpoint ticks it compares RAM with the dumps from MarioPartyDS-headless --dump-tick.

Needs a fresh melonDS start with "break on startup" for the ARM9 and gdb_server.py --stay-stopped,
because a reset ends the stub connection. Touches no save data and presses no keys.

Usage: python tools/compare_ticks_melonds.py <dump folder> <last tick> [checkpoint ...]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402

GAME_TICK = 0x02016D50
MODE_STATE = 0x020C9660
AREAS = [("main_ram", 0x02000000, 0x400000), ("itcm", 0x01FF8000, 0x8000),
         ("dtcm", 0x027E0000, 0x4000), ("palette", 0x05000000, 0x800), ("oam", 0x07000000, 0x800)]
# Written by the ARM7 or the firmware, not by the game: the stand-in only fills what the game reads.
ARM7_AREA_START = 0x02380000


def compare(gdb, folder, tick):
    path = os.path.join(folder, f"tick{tick}")
    print(f"--- tick {tick}")
    for name, base, size in AREAS:
        live = gdb.read(base, size)
        with open(os.path.join(path, f"{name}.bin"), "rb") as f:
            native = f.read()
        count = min(len(native), size, len(live))
        if name == "main_ram":
            count = min(count, ARM7_AREA_START - base)
        diffs = [i for i in range(count) if live[i] != native[i]]
        print(f"{name:9} {count - len(diffs)}/{count} bytes equal "
              f"({100 * (count - len(diffs)) / count:.3f}%)")
        runs = []
        for i in diffs:
            if runs and i - runs[-1][1] <= 16:
                runs[-1][1] = i
            else:
                runs.append([i, i])
        runs.sort(key=lambda r: r[0] - r[1])
        for s, e in runs[:8]:
            print(f"    {base + s:#010x}-{base + e + 1:#010x} melonDS "
                  f"{live[s:min(e + 1, s + 10)].hex(' ')} native "
                  f"{native[s:min(e + 1, s + 10)].hex(' ')}")
        if runs:
            print(f"    {len(runs)} differing ranges")


def main():
    folder, last = sys.argv[1], int(sys.argv[2])
    checkpoints = {int(a) for a in sys.argv[3:]}
    with Session() as gdb:
        gdb.interrupt()
        print("breakpoint:", gdb.set_break(GAME_TICK), flush=True)
        tick = 0
        last_scene = None
        while tick < last:
            gdb.resume()
            gdb.wait_stop(timeout=60.0)
            gdb.drain(quiet=0.02)
            gdb.halted_here = True
            tick += 1
            state = gdb.read(MODE_STATE, 16)
            scene = struct.unpack("<h", state[:2])[0]
            if scene != last_scene:
                last_scene = scene
                print(f"scene {scene} at game tick {tick}, mode {state[0xC]}, next {state[0xD]}",
                      flush=True)
            if tick in checkpoints:
                compare(gdb, folder, tick)
                print("ARM7 keys", gdb.read(0x027FFFA8, 2).hex(),
                      "touch", gdb.read(0x027FFFAA, 4).hex(),
                      "clock", gdb.read(0x027FFDE8, 8).hex(), flush=True)
        gdb.clear_break(GAME_TICK)
        gdb.resume()
    print(f"done at tick {tick}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
