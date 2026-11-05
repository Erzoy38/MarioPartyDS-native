#!/usr/bin/env python3
"""Log what the running game loads, by breakpoints on its graphics loaders.

At each hit it reads the arguments, reads the data they point to from RAM and looks it up
by hash in an index of my unpacked leaves. A hit therefore also tests my unpacking: data in
RAM must match a leaf byte for byte.

Appends to trace_loads.csv, one row per call. Needs tools/gdb_server.py running.
With --modes it also drives the game: it ends the current scene by clearing the flag
scene_manager waits on, and writes the next mode when the manager reads it. That can hang or
crash the game, which only costs an emulator restart.

Usage: python tools/trace_loads.py [seconds] [--max-hits N] [--modes 86,8c,2d-30] [--dwell S]
"""
import argparse
import collections
import csv
import hashlib
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify, unwrap  # noqa: E402
from decompress import DecodeError, huff_decompress, lz10_decompress  # noqa: E402
from gdb_client import Session  # noqa: E402
from render_screen import source_files  # noqa: E402

# address: (name, kind, register holding the data pointer)
LOADERS = {
    0x0200724C: ("load_palette", "palette", "r1"),
    0x020072E4: ("load_tiles", "tiles", "r2"),
    0x020074B0: ("load_map", "map", "r2"),
    0x02007404: ("load_map2", "map", "r2"),
    0x02004228: ("load_map3", "map", "r2"),
    0x02007368: ("load_tiles2", "tiles", "r2"),
    0x02007294: ("load_palette2", "palette", "r2"),
    0x020076B8: ("load_obj_palette", "palette", "r1"),
    0x02007744: ("load_obj_tiles", "tiles", "r1"),
    0x02069978: ("load_overlay", None, None),
}
MODE_STATE = 0x020C9660
READ_NEXT_MODE = 0x0201A460  # scene_manager: ldrb r0,[r1,#0xd]
STUCK_AFTER = 15.0
OUT = "trace_loads.csv"
INDEX_CACHE = "leaf_index.csv"


def digest(data):
    return hashlib.sha1(data).hexdigest()


def build_index():
    """{sha1: [leaf names]} for palettes, tile sets and maps. Cached in leaf_index.csv."""
    index = collections.defaultdict(list)
    if os.path.exists(INDEX_CACHE):
        with open(INDEX_CACHE, newline="") as f:
            for row in csv.DictReader(f):
                index[row["sha1"]].append(row["name"])
        return index
    rows = []
    for _rel, leaves in source_files():
        for name, data, _depth in leaves:
            if classify(data).split("-")[0] in ("palette", "tiles4", "tiles8", "map"):
                index[digest(data)].append(name)
                rows.append((digest(data), name))
    with open(INDEX_CACHE, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sha1", "name"])
        w.writerows(rows)
    return index


def leaf_size(kind, head):
    """Size of a leaf from its first word, or None if the header makes no sense."""
    word = struct.unpack("<I", head)[0]
    if kind == "palette":
        size = 4 + (word & 0xFFFF)
    elif kind == "map":
        size = 4 + (word & 0xFFFF)
        if (word & 0xFFFF) != (word >> 16 & 0xFF) * (word >> 24) * 2:
            return None
    else:
        size = 4 + (word >> 8) * (0x40 if word & 0xF else 0x20)
    return size if 4 < size <= 0x100000 else None


def read_leaf(gdb, kind, pointer):
    """Returns (data, note). Handles the wrapper word in front of some leaves."""
    head = gdb.read(pointer, 8)
    word, inner = struct.unpack("<II", head)
    # Wrapper: u32 (inner size << 8), low byte 0, around a normal leaf.
    if word & 0xFF == 0 and leaf_size(kind, head[4:]) == word >> 8:
        return gdb.read(pointer + 4, word >> 8), "wrapped"
    # Only tile sets use bits 4-7 for compression; in palettes and maps they are the size.
    # A map whose size field does not fit its width and height is packed (see load_map).
    method = word & 0xF0 if kind == "tiles" or (kind == "map" and leaf_size(kind, head[:4]) is None) else 0
    if method:
        # Still compressed in RAM; the loader unpacks it. The header gives the output size.
        output = word >> 8
        if method not in (0x10, 0x20) or not 0 < output <= 0x100000:
            return None, f"compressed type {method:#x}, header {head.hex()}"
        raw = gdb.read(pointer, min(output * 9 // 8 + 0x400, 0x120000))
        try:
            data = lz10_decompress(raw)[0] if method == 0x10 else huff_decompress(raw)[0]
        except DecodeError as exc:
            return None, f"unpack failed: {exc}"
        return unwrap(data), f"packed {method:#x}"
    size = leaf_size(kind, head[:4])
    if size is None:
        return None, f"bad header {head.hex()}"
    return unwrap(gdb.read(pointer, size)), ""


def parse_modes(text):
    modes = []
    for part in text.split(","):
        first, _, last = part.partition("-")
        modes += range(int(first, 16), int(last or first, 16) + 1)
    return modes


def force_scene_end(gdb):
    """Clear bit 0 at [main task + 0x48], the flag scene_manager waits on. Returns the task."""
    gdb.interrupt()
    task = gdb.u32(MODE_STATE + 8)
    if 0x02000000 <= task < 0x02400000:
        flags = gdb.u16(task + 0x48)
        gdb.write(task + 0x48, struct.pack("<H", flags & ~1))
    gdb.resume()
    gdb.halted_here = False
    return task


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seconds", nargs="?", type=float, default=30.0)
    ap.add_argument("--max-hits", type=int, default=5000)
    ap.add_argument("--modes", help="comma list of hex modes or ranges to jump through, e.g. 86,8c,2d-30")
    ap.add_argument("--dwell", type=float, default=6.0, help="seconds per mode when jumping")
    args = ap.parse_args()
    queue = parse_modes(args.modes) if args.modes else []
    sweeping = bool(queue)
    last_switch = 0.0
    forced_at = None
    log = []

    print("building leaf index...", flush=True)
    index = build_index()
    print(f"index: {len(index)} distinct leaves", flush=True)

    new_file = not os.path.exists(OUT)
    out = open(OUT, "a", newline="")
    writer = csv.writer(out)
    if new_file:
        writer.writerow(["time", "func", "lr", "engine", "layer_or_arg", "arg3", "pointer",
                         "kind", "size", "match", "names", "mode", "note"])

    tally = collections.Counter()
    with Session() as gdb:
        gdb.interrupt()
        for addr in LOADERS:
            reply = gdb.set_break(addr)
            if reply != b"OK":
                print(f"breakpoint at {addr:#x}: {reply!r}")
        if sweeping:
            print("jump breakpoint:", gdb.set_break(READ_NEXT_MODE), flush=True)
        print("breakpoints set", flush=True)
        end = time.time() + args.seconds
        try:
            gdb.resume()
            while time.time() < end and sum(tally.values()) < args.max_hits:
                now = time.time()
                if sweeping:
                    if forced_at is not None and now - forced_at > STUCK_AFTER:
                        print(f"STUCK: no scene change {STUCK_AFTER}s after forcing; "
                              f"last jumps {log[-3:]}", flush=True)
                        break
                    if forced_at is None and now - last_switch > args.dwell:
                        if not queue:
                            print("sweep finished", flush=True)
                            break
                        task = force_scene_end(gdb)
                        forced_at = time.time()
                        print(f"forced scene end, task {task:#x}", flush=True)
                try:
                    stop = gdb.wait_stop(timeout=min(0.5, max(end - time.time(), 0.1)))
                except (socket.timeout, TimeoutError):
                    continue
                gdb.drain(quiet=0.05)
                gdb.halted_here = True
                regs, _ = gdb.registers()
                pc = regs["pc"]
                if sweeping and pc == READ_NEXT_MODE:
                    old = gdb.read(MODE_STATE + 0xD, 1)[0]
                    mode = queue.pop(0) if queue else old
                    gdb.write(MODE_STATE + 0xD, bytes([mode]))
                    log.append(f"{mode:#x}")
                    print(f"jump: next mode was {old:#x}, now {mode:#x}", flush=True)
                    writer.writerow([f"{time.time():.2f}", "jump", "", "", "", "", "", "jump", "", "",
                                     f"{mode:#x}", f"{mode:#x}", ""])
                    out.flush()
                    last_switch = time.time()
                    forced_at = None
                    gdb.clear_break(pc)
                    gdb.step()
                    gdb.drain(quiet=0.05)
                    gdb.set_break(pc)
                    gdb.resume()
                    gdb.halted_here = False
                    continue
                loader = LOADERS.get(pc)
                if loader is None:
                    tally["stop elsewhere"] += 1
                    print(f"stop elsewhere: pc={pc:#x} {stop!r}", flush=True)
                    gdb.resume()
                    continue
                name, kind, reg = loader
                mode = gdb.read(0x020C966F, 1)[0]
                row = [f"{time.time():.2f}", name, f"{regs['lr']:#010x}", regs["r0"], regs["r1"],
                       regs["r3"]]
                if kind is None:
                    row += ["", "overlay", "", "", f"ov{regs['r1']:03}", f"{mode:#x}", ""]
                    tally["load_overlay"] += 1
                else:
                    pointer = regs[reg]
                    if pointer == 0:
                        data, note = None, "null"
                    else:
                        data, note = read_leaf(gdb, kind, pointer)
                    names = index.get(digest(data), []) if data else []
                    result = "yes" if names else ("unreadable" if data is None else "no")
                    if not names and 0x02000000 <= pointer < 0x020C2F58:
                        result = "in program"  # embedded in arm9.bin, not a data file
                    tally[f"{kind}: {result}"] += 1
                    row += [f"{pointer:#010x}", kind, len(data) if data else "", result,
                            " ".join(names[:6]), f"{mode:#x}", note]
                writer.writerow(row)
                out.flush()
                # Step off the breakpoint, then put it back.
                gdb.clear_break(pc)
                gdb.step()
                gdb.drain(quiet=0.05)
                gdb.set_break(pc)
                gdb.resume()
                gdb.halted_here = False
        finally:
            gdb.interrupt()
            for addr in LOADERS:
                gdb.clear_break(addr)
            gdb.clear_break(READ_NEXT_MODE)
            regs, _ = gdb.registers()
            mode = gdb.read(MODE_STATE, 16)
            print(f"at the end: pc={regs['pc']:#x} current mode {mode[0xF]:#x} next {mode[0xD]:#x}")
    out.close()
    print("jumps:", log)
    print("hits:", dict(sorted(tally.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
