#!/usr/bin/env python3
"""Render sprite frame sets to sheets in gfx/.

Frame set: u32 N, u32 offsets[N] from +4. Frame: u16 parts, 10 bytes per part: attr0,
attr1, attr2, s16 x, s16 y.

Pairing under test. Tiles: the set whose size fits exactly with a tile index unit of
32, 64, 128 or 256 bytes, nearest to the frames. Palette: see choose_palette.

Usage: python tools/render_sprites.py [--only substring] [--max-frames N]
Writes gfx/sprite_*.png and gfx_sprites.csv. Identical sheets are written once.
"""
import argparse
import collections
import csv
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from gfx import parse_palette, write_png  # noqa: E402
from render_screen import runs_of, source_files  # noqa: E402

SIZES = {
    (0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
    (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
    (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64),
}
UNITS = (32, 64, 128, 256)
CELL_GAP = 2


def parse_frames(data):
    count = struct.unpack_from("<I", data, 0)[0]
    offsets = struct.unpack_from(f"<{count}I", data, 4)
    if list(offsets) != sorted(offsets):
        return None
    frames = []
    for i, off in enumerate(offsets):
        start = off + 4
        end = offsets[i + 1] + 4 if i + 1 < count else len(data)
        if start + 2 > len(data):
            return None
        parts = struct.unpack_from("<H", data, start)[0]
        if end - start != 2 + parts * 10:
            return None
        frames.append([struct.unpack_from("<HHHhh", data, start + 2 + p * 10)
                       for p in range(parts)])
    return frames


def part_info(part):
    attr0, attr1, attr2, x, y = part
    shape, size = attr0 >> 14, attr1 >> 14
    if shape == 3:
        return None
    width, height = SIZES[(shape, size)]
    bpp = 8 if attr0 >> 13 & 1 else 4
    return {
        "w": width, "h": height, "bpp": bpp, "x": x, "y": y,
        "tile": attr2 & 0x3FF, "bank": attr2 >> 12,
        "hflip": attr1 >> 12 & 1, "vflip": attr1 >> 13 & 1,
    }


def bytes_needed(frames, unit):
    last = 0
    for frame in frames:
        for part in frame:
            info = part_info(part)
            if info:
                last = max(last, info["tile"] * unit + info["w"] * info["h"] * info["bpp"] // 8)
    return last


def frame_bpp(frames):
    kinds = {part_info(p)["bpp"] for f in frames for p in f if part_info(p)}
    return kinds.pop() if len(kinds) == 1 else None


def fit_unit(items, tile_pos, frames):
    size = len(items[tile_pos][1]) - 4
    for unit in UNITS:
        if bytes_needed(frames, unit) == size:
            return unit
    return None


def load_truth(path="sprite_tables.csv"):
    """Descriptors from overlays: {(file stem, frames): {(tiles, palette)}}.

    The main program names many files, so its hits are ambiguous and are left out.
    Language folders share a layout, so the key drops the folder.
    """
    truth = collections.defaultdict(set)
    if not os.path.exists(path):
        return truth
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["image"] == "arm9":
                continue
            stem = row["file"].split("/")[-1]
            truth[(stem, int(row["frames"]))].add((int(row["tiles"]), int(row["palette"])))
    return truth


def choose_tiles(items, pos, frames, frame_runs=None):
    """Returns (tile position, unit, how)."""
    bpp = frame_bpp(frames)
    if bpp is None:
        return None, None, "mixed"
    prefix = f"tiles{bpp}"
    candidates = [i for i, it in enumerate(items) if it[2].startswith(prefix)]
    if not candidates:
        return None, None, "no tiles"
    # Parallel lists: k-th frame set with k-th tile set, when the sizes agree.
    # Single-entry runs say nothing about order, so they fall through to "nearest".
    if frame_runs and pos in frame_runs and len(frame_runs[pos][0]) > 1:
        run, index = frame_runs[pos]
        same_length = {v[0] for v in runs_of(candidates).values() if len(v[0]) == len(run)}
        if same_length:
            nearest = min(same_length, key=lambda r: min(abs(r[0] - pos), abs(r[-1] - pos)))
            unit = fit_unit(items, nearest[index], frames)
            if unit:
                return nearest[index], unit, "exact"
    fits = []
    for i in candidates:
        size = len(items[i][1]) - 4
        for unit in UNITS:
            if bytes_needed(frames, unit) == size:
                fits.append((abs(i - pos), i < pos, unit, i))
    if fits:
        fits.sort()
        return fits[0][3], fits[0][2], "exact"
    big = [(abs(i - pos), i) for i in candidates
           if len(items[i][1]) - 4 >= bytes_needed(frames, 32)]
    if big:
        return min(big)[1], 32, "fallback"
    return None, None, "too small"


def choose_palette(items, tile_pos, frames):
    """Nearest palette before the tile set with enough colours for the banks used."""
    need = 0
    for frame in frames:
        for part in frame:
            info = part_info(part)
            if info and info["bpp"] == 4:
                need = max(need, (info["bank"] + 1) * 16)
    need = need or 16
    palettes = [i for i, it in enumerate(items) if it[2] == "palette"]
    enough = [i for i in palettes if struct.unpack_from("<H", items[i][1], 2)[0] >= need]
    pool = enough or palettes
    if not pool:
        return None
    # The usual layout is palette, tiles, frames, animation.
    if tile_pos - 1 in pool:
        return tile_pos - 1
    # Same position in its run when palette and tile runs have the same length.
    tile_runs = runs_of([i for i, it in enumerate(items) if it[2].startswith("tiles")])
    palette_runs = runs_of(palettes)
    run, index = tile_runs[tile_pos]
    starts = {r[0][0]: r[0] for r in palette_runs.values()}
    nearest = min(starts.values(), key=lambda r: min(abs(r[0] - tile_pos), abs(r[-1] - tile_pos)))
    if len(nearest) == len(run) and nearest[index] in pool:
        return nearest[index]
    before = [i for i in pool if i < tile_pos]
    return max(before) if before else min(pool, key=lambda i: abs(i - tile_pos))


def draw_frame(frame, tile_data, unit, palette):
    """Returns (w, h, rgba, out of range count) cropped to the frame's bounding box."""
    infos = [part_info(p) for p in frame]
    infos = [i for i in infos if i]
    if not infos:
        return None
    left = min(i["x"] for i in infos)
    top = min(i["y"] for i in infos)
    right = max(i["x"] + i["w"] for i in infos)
    bottom = max(i["y"] + i["h"] for i in infos)
    width, height = right - left, bottom - top
    out = bytearray(width * height * 4)
    colors = len(palette)
    bad = 0
    # Lower OAM numbers are in front, so draw in reverse.
    for info in reversed(infos):
        start = info["tile"] * unit
        tile_bytes = 64 if info["bpp"] == 8 else 32
        cols, rows = info["w"] // 8, info["h"] // 8
        bank = info["bank"] * 16 if info["bpp"] == 4 else 0
        for row in range(rows):
            for col in range(cols):
                tile_off = start + (row * cols + col) * tile_bytes
                raw = tile_data[tile_off:tile_off + tile_bytes]
                if len(raw) < tile_bytes:
                    bad += 1
                    continue
                for py in range(8):
                    for px in range(8):
                        if info["bpp"] == 8:
                            value = raw[py * 8 + px]
                        else:
                            byte = raw[py * 4 + px // 2]
                            value = byte >> 4 if px & 1 else byte & 15
                        if value == 0:
                            continue
                        gx = col * 8 + px
                        gy = row * 8 + py
                        if info["hflip"]:
                            gx = info["w"] - 1 - gx
                        if info["vflip"]:
                            gy = info["h"] - 1 - gy
                        color = bank + value
                        if color >= colors:
                            bad += 1
                            continue
                        dx = info["x"] - left + gx
                        dy = info["y"] - top + gy
                        pos = (dy * width + dx) * 4
                        r, g, b = palette[color]
                        out[pos:pos + 4] = bytes((r, g, b, 255))
    return width, height, bytes(out), bad


def build_sheet(rendered, columns=8):
    """Lay out frames in rows. Returns (w, h, rgba)."""
    rows = [rendered[i:i + columns] for i in range(0, len(rendered), columns)]
    width = max(sum(r[0] + CELL_GAP for r in row) for row in rows)
    height = sum(max(r[1] for r in row) + CELL_GAP for row in rows)
    canvas = bytearray(width * height * 4)
    oy = 0
    for row in rows:
        ox = 0
        for w, h, rgba, _bad in row:
            for y in range(h):
                start = (oy + y) * width * 4 + ox * 4
                canvas[start:start + w * 4] = rgba[y * w * 4:(y + 1) * w * 4]
            ox += w + CELL_GAP
        oy += max(r[1] for r in row) + CELL_GAP
    return width, height, bytes(canvas)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only")
    ap.add_argument("--max-frames", type=int, default=64)
    args = ap.parse_args()
    os.makedirs("gfx", exist_ok=True)

    totals = collections.Counter()
    rows = []
    seen = set()
    truth = load_truth()
    for name, leaves in source_files():
        if args.only and args.only not in name:
            continue
        groups = collections.defaultdict(list)
        for leaf, data, _depth in leaves:
            groups[leaf.rsplit("#", 1)[0]].append((leaf, data, classify(data)))
        for parent, items in groups.items():
            parsed = {}
            for pos, item in enumerate(items):
                if item[2] == "offsets":
                    frames = parse_frames(item[1])
                    if frames and any(frames):
                        parsed[pos] = frames
            frame_runs = runs_of(sorted(parsed))
            stem = parent.split("/")[-1]
            for pos, item in enumerate(items):
                frames = parsed.get(pos)
                if frames is None:
                    continue
                totals["frame sets"] += 1
                tile_pos, unit, how = choose_tiles(items, pos, frames, frame_runs)
                totals[f"tiles: {how}"] += 1
                palette_pos = choose_palette(items, tile_pos, frames) if tile_pos is not None else None

                # Where the game's own descriptor is known and unique, it decides.
                source = "rule"
                known = truth.get((stem, pos))
                if known and len(known) == 1:
                    true_tile, true_palette = next(iter(known))
                    totals["descriptor known"] += 1
                    totals["  rule tiles right" if tile_pos == true_tile else "  rule tiles wrong"] += 1
                    totals["  rule palette right" if palette_pos == true_palette else "  rule palette wrong"] += 1
                    if true_tile < len(items) and true_palette < len(items):
                        fitted = fit_unit(items, true_tile, frames)
                        if fitted:
                            tile_pos, unit, palette_pos = true_tile, fitted, true_palette
                            source = "descriptor"
                elif known:
                    # Several valid pairings, e.g. one palette per player colour. Show one.
                    totals["descriptor with several options"] += 1
                    for true_tile, true_palette in sorted(known):
                        if true_tile < len(items) and true_palette < len(items):
                            fitted = fit_unit(items, true_tile, frames)
                            if fitted:
                                tile_pos, unit, palette_pos = true_tile, fitted, true_palette
                                source = "descriptor-multi"
                                break
                if tile_pos is None:
                    continue
                if palette_pos is None:
                    totals["no palette"] += 1
                    continue
                palette = parse_palette(items[palette_pos][1])
                tile_data = items[tile_pos][1][4:]
                rendered = []
                bad = 0
                for frame in frames[:args.max_frames]:
                    result = draw_frame(frame, tile_data, unit, palette)
                    if result:
                        rendered.append(result)
                        bad += result[3]
                if not rendered:
                    continue
                totals["sets rendered"] += 1
                totals["sets with out-of-range values" if bad else "sets fully in range"] += 1
                width, height, rgba = build_sheet(rendered)
                digest = hashlib.sha1(rgba).hexdigest()
                png = ""
                if digest not in seen and any(rgba[3::4]):
                    seen.add(digest)
                    png = "gfx/sprite_" + item[0].replace("/", "_").replace("#", "_") + ".png"
                    write_png(png, width, height, rgba)
                    totals["unique sheets"] += 1
                rows.append([item[0], items[tile_pos][0], items[palette_pos][0], unit, how,
                             len(frames), bad, width, height, png, source])

    if not args.only:
        with open("gfx_sprites.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["frames", "tiles", "palette", "unit", "tiles_chosen", "frame_count",
                        "bad", "width", "height", "png", "source"])
            w.writerows(rows)
    for key, value in totals.most_common():
        print(f"{key:32}{value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
