#!/usr/bin/env python3
"""Plays every Free Play minigame online with four players, each in its own process.

Per game: a host and three Download Play guests start from the beginning, the host goes through the
real multiplayer menu (Minigame Mode, Free Play, Four-Player Match), everybody picks a character,
and the host selects the game by its tab (RIGHT) and list position (DOWN). All four then play with a
fixed mix of buttons, touches and blowing until the game ends.

Checked per game, for all four players: the minigame scene starts at the same exchange step, ends at
the same step, and the pictures 60 and 120 steps before the end (usually the winner screen) are
compared between the players. Players see their own view in some games, so a difference there is
reported, not judged.

Test aid: the host runs with --unlock-minigames 73, which marks all minigames as played in memory so
that Free Play lists them. The save file is only read.

Writes run/netmatrix/results.csv and contact sheets run/netmatrix/sheet_N.png (host picture 60 steps
before the end).

The five Puzzle Collection games are two-player games in the game itself; they run with a host and
one guest through Puzzle Mode.

Usage: python tools/net_minigame_matrix.py [--scenes 1,2,3] [--jobs 3] [--rounds 20]
"""
import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import write_png  # noqa: E402

ROM = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).nds"
SAVE = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1).sav"
EXE = os.path.join("native", "build", "MarioPartyDS-headless.exe")
ENTRIES = os.path.join("tools", "recomp_entries.txt")
OUT = os.path.join("run", "netmatrix")
BLOCKS_PER_TICK = 52600
GUEST_OFFSET = 2718   # a guest's game tick is the host's minus this, from scene 92 on
SELECT_TICK = 5600    # the Free Play list is up by then
PLAY_TICK = 6400      # instructions are up by then; from here everybody plays
PLAY_TICKS = 9000     # --play-ticks
TABS = {1: 0, 2: 1, 3: 2, 4: 3}  # id hundreds: 4-Player, 1-vs-3, 2-vs-2, Battle

HOST_MENU = ("700:touch;128;180,1000:A,1100:A,1200:A,1450:touch;128;108,1550:touch;223;178,"
             "2700:touch;223;178,3100:touch;128;116,3600:touch;170;55,3700:touch;223;178,"
             "4000:touch;44;50,4100:touch;223;178,4400:touch;176;83,4500:touch;223;178,"
             "4800:touch;75;36,4900:touch;223;178")
GUEST_CHARACTER = ["75;66", "184;36", "75;126"]  # Luigi, Peach, Yoshi

# Puzzle Mode (two players): Puzzle Collection, characters, then one of five icons.
PUZZLE_MENU = ("700:touch;128;180,1000:A,1100:A,1200:A,1450:touch;128;108,1550:touch;223;178,"
               "2700:touch;223;178,3100:touch;128;116,3600:touch;86;118,3700:touch;223;178,"
               "4100:touch;128;56,4200:touch;223;178,4600:touch;75;36,4700:touch;223;178")
PUZZLE_ICONS = {"601": "43;52", "602": "128;52", "603": "210;52", "604": "85;118", "605": "170;118"}
PUZZLE_SELECT_TICK = 5300
PUZZLE_PLAY_TICK = 5700


def free_play_games(rows):
    """The games in the Free Play tabs, with their tab and list position (order of the ids), and the
    puzzles of the Puzzle Collection."""
    games = []
    counters = {}
    for row in rows:
        group = int(re.match(r"\d", row["mg_id"]).group(0))
        if row["mg_id"] in PUZZLE_ICONS:
            games.append({**row, "tab": "puzzle", "position": PUZZLE_ICONS[row["mg_id"]]})
            continue
        if group not in TABS:
            continue
        counters[group] = counters.get(group, 0) + 1
        games.append({**row, "tab": TABS[group], "position": counters[group]})
    return games


def play_inputs(start, player):
    """Every 45 ticks: A, blowing, a touch, a direction. Touch spots and directions rotate, so each
    of them repeats every 8 or 4 rounds; written as --repeat to keep the command line short."""
    end = start + PLAY_TICKS
    args = ["--repeat", f"{start}:{end}:45:A:4", "--repeat", f"{start + 5}:{end}:45:MIC:6"]
    keys = ["RIGHT", "UP", "LEFT", "DOWN"]
    for k in range(8):
        n = player * 7 + k
        args += ["--repeat", f"{start + 15 + 45 * k}:{end}:360:touch;{40 + (n * 37) % 180};{30 + (n * 53) % 130}:8"]
    for k in range(4):
        args += ["--repeat", f"{start + 30 + 45 * k}:{end}:180:{keys[(player * 7 + k) % 4]}:10"]
    return args


def host_args(game, folder, port):
    args = [EXE, ROM, "--stop", "0xFFFFFFFC", "--max-blocks", str(3 * (PLAY_TICK + PLAY_TICKS) * BLOCKS_PER_TICK),
            "--name", "melonDS", "--save", SAVE, "--net-host", str(port), "--unlock-minigames", "73",
            "--dump", os.path.join(folder, "host"), "--exit-frames", os.path.join(folder, "host"),
            "--end-after-minigame", "1", "--dump-tick", str(PLAY_TICK + PLAY_TICKS - 20)]
    if game["tab"] == "puzzle":
        for item in PUZZLE_MENU.split(","):
            args += ["--input", item]
        args += ["--input", f"{PUZZLE_SELECT_TICK}:touch;{game['position']}",
                 "--input", f"{PUZZLE_SELECT_TICK + 100}:touch;223;178"]
        return args + play_inputs(PUZZLE_PLAY_TICK, 0)
    for item in HOST_MENU.split(","):
        args += ["--input", item]
    tick = SELECT_TICK
    for _ in range(game["tab"]):
        args += ["--input", f"{tick}:RIGHT:4"]
        tick += 30
    tick = SELECT_TICK + 100
    for _ in range(game["position"] - 1):
        args += ["--input", f"{tick}:DOWN:4"]
        tick += 12
    args += ["--input", f"{tick + 60}:touch;223;178"]
    return args + play_inputs(PLAY_TICK, 0)


def guest_args(number, folder, port, puzzle=False):
    name = f"guest{number}"
    args = [EXE, ROM, "--stop", "0xFFFFFFFC", "--max-blocks", str(3 * (PLAY_TICK + PLAY_TICKS) * BLOCKS_PER_TICK),
            "--name", f"Guest{number}", "--boot-type", "2", "--net-guest", str(port),
            "--dump", os.path.join(folder, name), "--exit-frames", os.path.join(folder, name),
            "--end-after-minigame", "1", "--repeat", "400:800:50:touch;128;116",
            "--input", f"{1900 if puzzle else 2100}:touch;{GUEST_CHARACTER[number - 1]}",
            "--input", f"{2000 if puzzle else 2200}:touch;223;178"]
    return args + play_inputs((PUZZLE_PLAY_TICK if puzzle else PLAY_TICK) - GUEST_OFFSET, number)


def run_game(game, port):
    folder = os.path.join(OUT, f"scene{int(game['scene']):03}")
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(folder):
        path = os.path.join(folder, name)
        if os.path.isdir(path):
            shutil.rmtree(path)
    host = subprocess.Popen(host_args(game, folder, port), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    puzzle = game["tab"] == "puzzle"
    guests = [subprocess.Popen(guest_args(n, folder, port, puzzle), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
              for n in ((1,) if puzzle else (1, 2, 3))]
    outputs = []
    for process in [host] + guests:
        try:
            out, _ = process.communicate(timeout=2400)
        except subprocess.TimeoutExpired:
            process.kill()
            out, _ = process.communicate()
            out += "\nABORT: test timeout\n"
        outputs.append(out)
    for name, out in zip(["host", "guest1", "guest2", "guest3"], outputs):
        with open(os.path.join(folder, f"{name}.txt"), "w", encoding="utf-8") as f:
            f.write(out)
    return game, outputs


def recompile():
    subprocess.run([sys.executable, os.path.join("tools", "recomp.py")], capture_output=True, check=True)
    build = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join("native", "build.ps1")], capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout[-2000:])
        raise SystemExit("build failed")


def missing_lines(outputs):
    found = set()
    for out in outputs:
        match = re.search(r"^MISSING ([0-9a-f]+ \d(?: ov\d+)?)", out, re.M)
        if match:
            found.add(match.group(1))
    return found


def scene_steps(output, scene):
    """Step at which `scene` starts and the step of the next scene change after it."""
    changes = [(int(s), int(step)) for s, step in re.findall(r"^scene (-?\d+) at game tick \d+, step (\d+)", output, re.M)]
    for index, (number, step) in enumerate(changes):
        if number == scene:
            end = changes[index + 1][1] if index + 1 < len(changes) else None
            return step, end
    return None, None


def frame_difference(a, b, half):
    size = len(a) // 2
    lo, hi = (0, size) if half == 0 else (size, len(a))
    return sum(1 for i in range(lo, hi, 4) if a[i:i + 3] != b[i:i + 3])


def compare_exit_frames(folder, scene):
    """Largest number of differing pixels against the host, per screen, over both exit pictures."""
    host_dir = os.path.join(folder, "host")
    names = [n for n in os.listdir(host_dir) if n.startswith(f"exit{scene}_")] if os.path.isdir(host_dir) else []
    if not names:
        return None, None, []
    worst_top = worst_bottom = 0
    for name in names:
        with open(os.path.join(host_dir, name), "rb") as f:
            reference = f.read()
        for guest in ("guest1", "guest2", "guest3"):
            if not os.path.isdir(os.path.join(folder, guest)):
                continue
            path = os.path.join(folder, guest, name)
            if not os.path.exists(path):
                return None, None, names
            with open(path, "rb") as f:
                other = f.read()
            worst_top = max(worst_top, frame_difference(reference, other, 0))
            worst_bottom = max(worst_bottom, frame_difference(reference, other, 1))
    return worst_top, worst_bottom, sorted(names)


def sheet(rows, number):
    columns = 8
    cell_w, cell_h = 128, 192
    height = ((len(rows) + columns - 1) // columns) * cell_h
    pixels = bytearray(columns * cell_w * height * 4)
    for index, row in enumerate(rows):
        if not row["picture"]:
            continue
        with open(row["picture"], "rb") as f:
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenes")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--play-ticks", type=int, default=PLAY_TICKS)
    args = ap.parse_args()
    globals()["PLAY_TICKS"] = args.play_ticks

    with open("minigames.csv", newline="", encoding="utf-8") as f:
        games = free_play_games(list(csv.DictReader(f)))
    if args.scenes:
        wanted = {int(s) for s in args.scenes.split(",")}
        games = [g for g in games if int(g["scene"]) in wanted]
    os.makedirs(OUT, exist_ok=True)

    results = {}
    pending = games
    for round_number in range(args.rounds):
        with ThreadPoolExecutor(args.jobs) as pool:
            # One port per game: games running side by side must not share one.
            finished = list(pool.map(lambda game: run_game(game, 47100 + int(game["scene"])), pending))
        missing = set()
        retry = []
        for game, outputs in finished:
            lines = missing_lines(outputs)
            if lines:
                missing |= lines
                retry.append(game)
            else:
                results[int(game["scene"])] = outputs
        print(f"round {round_number + 1}: ran {len(pending)}, missing code in {len(retry)}: {sorted(missing)}", flush=True)
        if not missing:
            break
        known = set()
        if os.path.exists(ENTRIES):
            with open(ENTRIES) as f:
                known = {line.strip() for line in f}
        if not missing - known:
            print("code still missing after being added:", sorted(missing), flush=True)
            for game in retry:
                results[int(game["scene"])] = ["ABORT: code stays missing"] * 4
            break
        with open(ENTRIES, "a") as f:
            for line in sorted(missing - known):
                f.write(line + "\n")
        recompile()
        pending = retry

    rows = []
    for game in games:
        scene = int(game["scene"])
        folder = os.path.join(OUT, f"scene{scene:03}")
        outputs = results.get(scene, [""] * (2 if game["tab"] == "puzzle" else 4))
        steps = [scene_steps(out, scene) for out in outputs]
        starts = {s for s, _ in steps}
        ends = {e for _, e in steps}
        ended = all("minigame ended at step" in out for out in outputs)
        aborts = [re.search(r"^ABORT: (.*)$", out, re.M) for out in outputs]
        early_abort = next((a.group(1) for a, out in zip(aborts, outputs)
                            if a and "minigame ended at step" not in out), "")
        top, bottom, names = compare_exit_frames(folder, scene)
        entered = None not in starts and len(starts) == 1
        same_end = None not in ends and len(ends) == 1
        status = "ok" if entered and same_end and ended and not early_abort else "fail"
        picture = os.path.join(folder, "host", names[0]) if names else ""
        rows.append({
            "scene": scene, "mg": game["mg_id"], "name": game["UK"], "tab": game["tab"],
            "position": game["position"], "status": status, "reason": early_abort,
            "start_steps": "/".join(str(s) for s, _ in steps), "end_steps": "/".join(str(e) for _, e in steps),
            "exit_pixels_top": "" if top is None else top, "exit_pixels_bottom": "" if bottom is None else bottom,
            "picture": picture, "controls": game["controls_uk"],
        })

    with open(os.path.join(OUT, "results.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    for number in range(0, len(rows), 32):
        print("sheet:", sheet(rows[number:number + 32], number // 32))
    ok = sum(r["status"] == "ok" for r in rows)
    print(f"{ok} of {len(rows)} started and ended at the same step for all players")
    for row in rows:
        if row["status"] != "ok":
            print(f"  scene {row['scene']} mg_{row['mg']} {row['name']}: start {row['start_steps']} "
                  f"end {row['end_steps']} {row['reason']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
