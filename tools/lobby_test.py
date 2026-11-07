"""Tests the lobby server and the way from a room to a running game.

Usage: python tools/lobby_test.py [--rules] [--game] [--port N]

--rules  the protocol rules of the server (tools/lobby_rules.py), with MarioPartyDS-headless as the
         real client.
--game   one server and four MarioPartyDS-headless processes: the host creates a room, three guests
         join with its code, the host starts, all four boot. The host goes through the real menu to
         Free Play; the characters are not touched by the script but tapped by the program from the
         lobby choice. Then the first minigame of the list is played; all four must enter and leave
         it at the same step.
Without options both run. Writes run/lobbytest/.
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lobby_rules as rules  # noqa: E402
import net_minigame_matrix as matrix  # noqa: E402

EXE = matrix.EXE
ROM = matrix.ROM
OUT = rules.OUT
start_server = rules.start_server
rules.CLIENT = [EXE, ROM]


# --- Game through the lobby ---
GUESTS = (("Guest1", "Luigi"), ("Guest2", "Peach"), ("Guest3", "Yoshi"))
PICK_CHECK_TICK = 4890  # host tick: all four have picked, the host has not confirmed yet


def host_menu_without_character():
    items = [i for i in matrix.HOST_MENU.split(",") if not i.startswith("4800:")]
    args = []
    for item in items:
        args += ["--input", item]
    return args


def run_game(port, play_ticks):
    folder = os.path.join(OUT, "game")
    os.makedirs(folder, exist_ok=True)
    code_file = os.path.join(folder, "code.txt")
    if os.path.exists(code_file):
        os.remove(code_file)
    server = start_server(port)
    end = matrix.PLAY_TICK + play_ticks
    limit = str(3 * end * matrix.BLOCKS_PER_TICK)
    host = [EXE, ROM, "--stop", "0xFFFFFFFC", "--max-blocks", limit, "--name", "melonDS", "--save", matrix.SAVE,
            "--lobby", f"127.0.0.1:{port}", "--character", "Mario", "--create", code_file,
            "--unlock-minigames", "73", "--dump", os.path.join(folder, "host"),
            "--frame-range", f"{PICK_CHECK_TICK}:{PICK_CHECK_TICK}",
            "--exit-frames", os.path.join(folder, "host"), "--end-after-minigame", "1"]
    host += host_menu_without_character()
    play = matrix.PLAY_TICKS
    matrix.PLAY_TICKS = play_ticks
    host += matrix.play_inputs(matrix.PLAY_TICK, 0)
    processes = [subprocess.Popen(host, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)]
    for number, (name, character) in enumerate(GUESTS, start=1):
        guest = [EXE, ROM, "--stop", "0xFFFFFFFC", "--max-blocks", limit, "--name", name,
                 "--lobby", f"127.0.0.1:{port}", "--character", character, "--join", "@" + code_file,
                 "--dump", os.path.join(folder, f"guest{number}"),
                 "--exit-frames", os.path.join(folder, f"guest{number}"), "--end-after-minigame", "1",
                 "--repeat", "400:800:50:touch;128;116", "--input", "2200:touch;223;178"]
        guest += matrix.play_inputs(matrix.PLAY_TICK - matrix.GUEST_OFFSET, number)
        processes.append(subprocess.Popen(guest, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
        time.sleep(0.5)
    matrix.PLAY_TICKS = play
    outputs = []
    for process in processes:
        try:
            out, _ = process.communicate(timeout=2400)
        except subprocess.TimeoutExpired:
            process.kill()
            out, _ = process.communicate()
            out += "\nABORT: test timeout\n"
        outputs.append(out)
    server.kill()
    names = ["host", "guest1", "guest2", "guest3"]
    for name, out in zip(names, outputs):
        with open(os.path.join(folder, f"{name}.txt"), "w", encoding="utf-8") as f:
            f.write(out)
    missing = matrix.missing_lines(outputs)
    if missing:
        return None, missing
    failures = 0
    expected = ["Mario"] + [c for _, c in GUESTS]
    # With four players the game skips the match-style question, so the host's script reaches the
    # list early and takes its first entry; whichever minigame it is, all must play the same one.
    scenes = [int(s) for s in re.findall(r"^scene (\d+) at", outputs[0], re.M) if 1 <= int(s) <= 77]
    scene = scenes[0] if scenes else None
    print(f"minigame: scene {scene}; character screen picture: {folder}/host/frames/{PICK_CHECK_TICK}.rgba")
    for name, out, character in zip(names, outputs, expected):
        player = re.search(r"^lobby: player (\d) of (\d), (\w+)", out, re.M)
        picked = re.findall(r"^net: picks (\w+) at step (\d+)", out, re.M)
        start, finish = matrix.scene_steps(out, scene) if scene else (None, None)
        ended = "minigame ended" in out
        print(f"{name}: {player.group(0) if player else 'no lobby start'}; picks {picked}; "
              f"minigame steps {start}..{finish}; ended {ended}")
        if not player or player.group(3) != character or [p[0] for p in picked] != [character] or not ended:
            failures += 1
    steps = {matrix.scene_steps(out, scene) if scene else (None, None) for out in outputs}
    if len(steps) != 1 or None in next(iter(steps)):
        print(f"minigame steps differ: {steps}")
        failures += 1
    return failures, set()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rules", action="store_true")
    parser.add_argument("--game", action="store_true")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--play-ticks", type=int, default=3000)
    args = parser.parse_args()
    both = not args.rules and not args.game
    failures = 0
    if args.rules or both:
        failures += rules.run_rules(args.port)
    if args.game or both:
        for round_number in range(1, 20):
            result, missing = run_game(args.port + 10, args.play_ticks)
            if result is not None:
                failures += result
                break
            print(f"round {round_number}: missing code {sorted(missing)}")
            with open(matrix.ENTRIES, "a") as f:
                for line in sorted(missing):
                    f.write(line + "\n")
            matrix.recompile()
    print("all passed" if failures == 0 else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
