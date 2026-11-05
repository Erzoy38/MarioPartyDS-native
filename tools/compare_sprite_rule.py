#!/usr/bin/env python3
"""Compare the sprite pairing rule with the overlay descriptors, by content.

Renders disagreeing cases both ways into a scratch folder for viewing.
Usage: python tools/compare_sprite_rule.py <out dir> [count]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_walk import classify  # noqa: E402
from gfx import parse_palette, write_png  # noqa: E402
from render_screen import runs_of, source_files  # noqa: E402
from render_sprites import (build_sheet, choose_palette, choose_tiles, draw_frame, fit_unit,  # noqa: E402
                            load_truth, parse_frames)


def main():
    out = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    os.makedirs(out, exist_ok=True)
    truth = load_truth()
    tally = collections.Counter()
    by_file = collections.Counter()
    written = 0
    for name, leaves in source_files():
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
            for pos, frames in parsed.items():
                known = truth.get((stem, pos))
                if not known or len(known) != 1:
                    continue
                true_tile, true_palette = next(iter(known))
                if true_tile >= len(items) or true_palette >= len(items):
                    tally["descriptor index past the container"] += 1
                    continue
                tally["cases"] += 1
                tile_pos, unit, _how = choose_tiles(items, pos, frames, frame_runs)
                palette_pos = choose_palette(items, tile_pos, frames) if tile_pos is not None else None
                same_tile = tile_pos is not None and items[tile_pos][1] == items[true_tile][1]
                same_palette = palette_pos is not None and items[palette_pos][1] == items[true_palette][1]
                tally["tile data identical" if same_tile else "tile data differs"] += 1
                tally["palette data identical" if same_palette else "palette data differs"] += 1
                if stem.startswith("mg_"):
                    tally["minigames: cases"] += 1
                    tally["minigames: tiles identical" if same_tile else "minigames: tiles differ"] += 1
                    tally["minigames: palette identical" if same_palette else "minigames: palette differs"] += 1
                if not same_tile or not same_palette:
                    by_file[stem] += 1
                true_unit = fit_unit(items, true_tile, frames)
                colors = len(parse_palette(items[true_palette][1])) if items[true_palette][2] == "palette" else 0
                tally["descriptor palette kind ok" if colors else "descriptor palette kind wrong"] += 1
                if (not same_tile or not same_palette) and written < limit and true_unit and colors:
                    written += 1
                    for label, t, u, p in (("rule", tile_pos, unit, palette_pos),
                                           ("game", true_tile, true_unit, true_palette)):
                        if t is None or p is None:
                            continue
                        palette = parse_palette(items[p][1])
                        rendered = []
                        for frame in frames[:24]:
                            result = draw_frame(frame, items[t][1][4:], u, palette)
                            if result:
                                rendered.append(result)
                        if rendered:
                            w, h, rgba = build_sheet(rendered)
                            safe = items[pos][0].replace("/", "_").replace("#", "_")
                            write_png(os.path.join(out, f"{safe}_{label}.png"), w, h, rgba)
    for key, value in sorted(tally.items()):
        print(f"{key:36}{value}")
    print("files with most disagreements:", by_file.most_common(12))
    return 0


if __name__ == "__main__":
    sys.exit(main())
