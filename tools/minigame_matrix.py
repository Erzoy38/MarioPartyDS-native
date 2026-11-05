#!/usr/bin/env python3
"""Starts every minigame in the native run and reports how each one goes.

Each game runs in its own process, entered through a shortcut (--force-mode), with a fixed mix of
button presses and touches. Code that turns out to be missing is translated and the round repeats.
Writes run/matrix/results.csv and contact sheets run/matrix/sheet_N.png with one picture per game.

This shows whether a game starts, runs and draws. It does not show that a game plays correctly:
the shortcut skips the menus, and the input is not aimed at anything.

Usage: python tools/minigame_matrix.py [--scenes 1,2,3] [--jobs 6] [--ticks 1500] [--rounds 40]
"""
import argparse
import csv
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import write_png  # noqa: E402

ROM = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds"
SAVE = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).sav"
EXE = os.path.join("native", "build", "MarioPartyDS-headless.exe")
ENTRIES = os.path.join("tools", "recomp_entries.txt")
OUT = os.path.join("run", "matrix")
BLOCKS_PER_TICK = 52600
ENTER_TICK = 770  # the scene starts here after the title is touched at tick 700


def inputs_for(ticks):
    args = ["--input", "700:touch;128;180"]
    start = ENTER_TICK + 60
    end = ENTER_TICK + ticks
    keys = ["RIGHT", "UP", "LEFT", "DOWN"]
    tick = start
    n = 0
    while tick < end:
        args += ["--input", f"{tick}:A:4"]
        args += ["--input", f"{tick + 5}:MIC:6"]
        args += ["--input", f"{tick + 15}:touch;{40 + (n * 37) % 180};{30 + (n * 53) % 130}:8"]
        args += ["--input", f"{tick + 30}:{keys[n % 4]}:10"]
        tick += 45
        n += 1
    return args


def run_scene(scene, mode, ticks):
    folder = os.path.join(OUT, f"scene{scene:03}")
    shots = [ENTER_TICK + ticks // 3, ENTER_TICK + ticks * 2 // 3, ENTER_TICK + ticks - 10]
    args = [EXE, ROM, "--stop", "0xFFFFFFFC", "--max-blocks", str((ENTER_TICK + ticks) * BLOCKS_PER_TICK),
            "--name", "melonDS", "--save", SAVE, "--dump", folder, "--force-mode", hex(mode)]
    for shot in shots:
        args += ["--dump-tick", str(shot)]
    args += inputs_for(ticks)
    done = subprocess.run(args, capture_output=True, text=True, timeout=1200)
    return scene, done.returncode, done.stdout, shots


def recompile():
    subprocess.run([sys.executable, os.path.join("tools", "recomp.py")], capture_output=True, check=True)
    build = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join("native", "build.ps1")], capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout[-2000:])
        raise SystemExit("build failed")


def missing_line(output):
    match = re.search(r"^MISSING ([0-9a-f]+ \d(?: ov\d+)?)", output, re.M)
    return match.group(1) if match else None


def sheet(rows, shots_of, number):
    columns = 8
    cell_w, cell_h = 128, 192
    height = ((len(rows) + columns - 1) // columns) * cell_h
    pixels = bytearray(columns * cell_w * height * 4)
    for index, row in enumerate(rows):
        path = os.path.join(OUT, f"scene{row['scene']:03}", f"tick{shots_of[row['scene']][1]}", "frame.rgba")
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            frame = f.read()
        ox = (index % columns) * cell_w
        oy = (index // columns) * cell_h
        for y in range(cell_h):
            for x in range(cell_w):
                a = (y * 2 * 256 + x * 2) * 4
                b = ((oy + y) * columns * cell_w + ox + x) * 4
                pixels[b:b + 4] = frame[a:a + 4]
    name = os.path.join(OUT, f"sheet_{number}.png")
    write_png(name, columns * cell_w, height, bytes(pixels))
    return name


def distinct_frames(scene, shots):
    frames = set()
    for shot in shots:
        path = os.path.join(OUT, f"scene{scene:03}", f"tick{shot}", "frame.rgba")
        if os.path.exists(path):
            with open(path, "rb") as f:
                frames.add(hash(f.read()))
    return len(frames)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenes")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--ticks", type=int, default=1500)
    ap.add_argument("--rounds", type=int, default=40)
    args = ap.parse_args()

    with open("minigames.csv", newline="", encoding="utf-8") as f:
        games = list(csv.DictReader(f))
    wanted = {int(s) for s in args.scenes.split(",")} if args.scenes else None
    games = [g for g in games if wanted is None or int(g["scene"]) in wanted]
    os.makedirs(OUT, exist_ok=True)

    results = {}
    shots_of = {}
    pending = games
    for round_number in range(args.rounds):
        with ThreadPoolExecutor(args.jobs) as pool:
            finished = list(pool.map(
                lambda g: run_scene(int(g["scene"]), int(g["mode"], 16), args.ticks), pending))
        missing = set()
        retry = []
        by_scene = {int(g["scene"]): g for g in pending}
        for scene, code, output, shots in finished:
            shots_of[scene] = shots
            line = missing_line(output)
            if line:
                missing.add(line)
                retry.append(by_scene[scene])
                continue
            results[scene] = (code, output)
        print(f"round {round_number + 1}: ran {len(pending)}, missing code in {len(retry)}: {sorted(missing)}", flush=True)
        if not missing:
            break
        known = set()
        if os.path.exists(ENTRIES):
            with open(ENTRIES) as f:
                known = {line.strip() for line in f}
        with open(ENTRIES, "a") as f:
            for line in sorted(missing - known):
                f.write(line + "\n")
        if not missing - known:
            print("code still missing after being added:", sorted(missing), flush=True)
            for g in retry:
                results[int(g["scene"])] = (3, "ABORT: code stays missing (other overlay?)")
            break
        recompile()
        pending = retry

    rows = []
    for game in games:
        scene = int(game["scene"])
        code, output = results.get(scene, (-1, ""))
        abort = re.search(r"^ABORT: (.*)$", output, re.M)
        ticks = re.search(r"game ticks (\d+)", output)
        progress = re.search(r"scene changes (\d+), scene (-?\d+), mode (\d+)", output)
        lines_3d = re.search(r"3D lines (\d+)", output)
        notes = re.search(r"sound: (\d+) notes", output)
        rows.append({
            "scene": scene,
            "mg": game["mg_id"],
            "name": game["UK"],
            "status": "abort" if abort else ("ok" if code == 4 else f"exit {code}"),
            "reason": abort.group(1) if abort else "",
            "game_ticks": ticks.group(1) if ticks else "",
            "end_scene": progress.group(2) if progress else "",
            "lines_3d": lines_3d.group(1) if lines_3d else "",
            "notes": notes.group(1) if notes else "",
            "different_pictures": distinct_frames(scene, shots_of.get(scene, [])),
            "controls": game["controls_uk"],
        })

    with open(os.path.join(OUT, "results.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    for number in range(0, len(rows), 32):
        print("sheet:", sheet(rows[number:number + 32], shots_of, number // 32))
    ok = sum(r["status"] == "ok" for r in rows)
    print(f"{ok} of {len(rows)} ran to the end of the test time")
    for row in rows:
        if row["status"] != "ok" or row["end_scene"] != str(row["scene"]):
            print(f"  scene {row['scene']} mg_{row['mg']} {row['name']}: {row['status']} {row['reason']} ended in scene {row['end_scene']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
