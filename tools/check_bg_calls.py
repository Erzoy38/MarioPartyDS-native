#!/usr/bin/env python3
"""Compare my map/palette/tile pairing with what the game's code actually passes.

Loaders in the main program:
  0x0200724C palette(engine r0, data r1, offset r2)
  0x020072E4 tiles(engine r0, layer r1, data r2, offset r3)
  0x020074B0 map(engine r0, layer r1, data r2, ...)
  0x02009A38 load_bg(engine r0, layer r1, palette r2, tiles r3, map [sp], ...)
Data comes from get_member (0x02009994)(container, index).

For each function that calls them, this follows constants through registers and stack
slots in a straight line. A map is compared with the tiles loaded for the same engine
and layer, and the palette for the same engine. Conditional code and loops can defeat
this, so only cases with constant indices are counted.

Usage: python tools/check_bg_calls.py
"""
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import decode_arm  # noqa: E402
from nds_image import all_code_images  # noqa: E402

PALETTE, TILES, MAP, LOAD_BG = 0x0200724C, 0x020072E4, 0x020074B0, 0x02009A38
GET_MEMBER = 0x02009994
NAMES = {f"r{i}": i for i in range(13)}
NAMES.update(sp=13, lr=14, pc=15)


def read_string(data, base, addr):
    offset = addr - base
    if not 0 <= offset < len(data):
        return None
    text = data[offset:data.find(b"\0", offset)]
    if len(text) >= 4 and all(0x20 <= b < 0x7F for b in text):
        return text.decode("ascii")
    return None


def member_of(value):
    if not value or value[0] != "member":
        return None
    container, index = value[1], value[2]
    if not index or index[0] != "const":
        return None
    path = container[1] if container and container[0] == "file" else None
    return path, index[1]


def const_of(value):
    return value[1] if value and value[0] == "const" else None


def simulate(data, base, start, end):
    """Walk one function. Returns a list of (kind, engine, layer, member) events."""
    regs = {}
    stack = {}
    events = []
    for pos in range(start, end):
        word = struct.unpack_from("<I", data, pos * 4)[0]
        insn = decode_arm(word, base + pos * 4)
        if insn.undefined:
            continue
        if insn.cond:
            if insn.rd is not None:
                regs.pop(insn.rd, None)
            continue
        ops = insn.ops.split(",")
        if insn.mnem == "mov" and len(ops) == 2 and ops[1].startswith("#"):
            regs[insn.rd] = ("const", int(ops[1][1:], 0))
        elif insn.mnem == "mov" and len(ops) == 2 and ops[1] in NAMES:
            value = regs.get(NAMES[ops[1]])
            if value is None:
                regs.pop(insn.rd, None)
            else:
                regs[insn.rd] = value
        elif insn.mnem == "ldr" and insn.literal is not None:
            offset = insn.literal - base
            if 0 <= offset <= len(data) - 4:
                value = struct.unpack_from("<I", data, offset)[0]
                text = read_string(data, base, value)
                regs[insn.rd] = ("path", text) if text else ("const", value)
            else:
                regs.pop(insn.rd, None)
        elif insn.mnem == "str" and "[sp" in insn.ops:
            if "!" in insn.ops or "-" in insn.ops or "],#" in insn.ops:
                # Writeback changes sp, which this simple model does not track.
                stack.clear()
            else:
                slot = 0
                if "#" in insn.ops:
                    slot = int(insn.ops.split("#")[1].rstrip("]"), 0)
                stack[slot] = regs.get(NAMES.get(ops[0]))
        elif insn.call:
            target = insn.target
            if target == GET_MEMBER:
                result = ("member", regs.get(0), regs.get(1))
            else:
                result = None
                if target == PALETTE:
                    events.append(("palette", const_of(regs.get(0)), None, member_of(regs.get(1))))
                elif target == TILES:
                    events.append(("tiles", const_of(regs.get(0)), const_of(regs.get(1)),
                                   member_of(regs.get(2))))
                elif target == MAP:
                    events.append(("map", const_of(regs.get(0)), const_of(regs.get(1)),
                                   member_of(regs.get(2))))
                elif target == LOAD_BG:
                    engine, layer = const_of(regs.get(0)), const_of(regs.get(1))
                    events.append(("palette", engine, None, member_of(regs.get(2))))
                    events.append(("tiles", engine, layer, member_of(regs.get(3))))
                    events.append(("map", engine, layer, member_of(stack.get(0))))
                path = regs.get(0)
                # Loading returns a handle; unpacking the handle keeps it the same file.
                if path and path[0] in ("path", "file"):
                    result = ("file", path[1])
            for reg in (0, 1, 2, 3, 12, 14):
                regs.pop(reg, None)
            if result:
                regs[0] = result
        elif insn.clobbers_all:
            regs.clear()
        elif insn.rd is not None:
            regs.pop(insn.rd, None)
    return events


def function_starts(data):
    count = len(data) // 4
    starts = []
    for i in range(count):
        word = struct.unpack_from("<I", data, i * 4)[0]
        if (word & 0xFFFF4000) == 0xE92D4000:
            starts.append(i)
    return starts


def overlay_name(tag, base, data):
    """When an overlay names exactly one graphics file, use it for unresolved paths."""
    return None


def main():
    with open("assets.csv", newline="") as f:
        kinds = {r["name"]: r["kind"] for r in csv.DictReader(f)}
    with open("gfx_screens.csv", newline="") as f:
        pairing = {r["map"]: r for r in csv.DictReader(f)}

    tally = collections.Counter()
    disagree = []
    for tag, base, data in all_code_images():
        starts = function_starts(data)
        for n, start in enumerate(starts):
            end = starts[n + 1] if n + 1 < len(starts) else len(data) // 4
            if end - start > 3000:
                end = start + 3000
            events = simulate(data, base, start, end)
            if not events:
                continue
            for i, (kind, engine, layer, member) in enumerate(events):
                if kind != "map":
                    continue
                tally["map loads"] += 1
                if member is None:
                    tally["map index not constant"] += 1
                    continue
                path = member[0]
                if path is None:
                    tally["map file not resolved (language table or stored pointer)"] += 1
                    continue
                rel = path[5:] if path.startswith("data/") else path.lstrip("/")
                map_name = f"{rel}#{member[1]:03}"
                if kinds.get(map_name) != "map":
                    tally["map member is not a map leaf"] += 1
                    disagree.append((tag, hex(base + start * 4), map_name, kinds.get(map_name)))
                    continue
                tally["map member verified as map leaf"] += 1
                tiles = [e for e in events if e[0] == "tiles" and e[1] == engine and e[2] == layer and e[3]]
                palettes = [e for e in events if e[0] == "palette" and e[1] == engine and e[3]]
                mine = pairing.get(map_name)
                if mine is None:
                    tally["map not in my render list"] += 1
                    continue
                if tiles:
                    tile_name = f"{rel}#{tiles[-1][3][1]:03}"
                    # Same index exists in the same container; compare by index.
                    key = "tiles agree" if tile_name == mine["tiles"] else "tiles differ"
                    tally[key] += 1
                    tally[f"{key} [{mine['tiles_chosen']}]"] += 1
                    if key == "tiles differ":
                        disagree.append((tag, hex(base + start * 4), map_name,
                                         f"game {tile_name} mine {mine['tiles']}"))
                if palettes:
                    pal_name = f"{rel}#{palettes[-1][3][1]:03}"
                    key = "palette agrees" if pal_name == mine["palette"] else "palette differs"
                    tally[key] += 1
                    if key == "palette differs":
                        disagree.append((tag, hex(base + start * 4), map_name,
                                         f"game {pal_name} mine {mine['palette']}"))
    for key, value in sorted(tally.items()):
        print(f"{key:58}{value}")
    print(f"\ndisagreements listed: {len(disagree)}")
    for item in disagree[:30]:
        print(" ", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
