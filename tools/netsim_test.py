"""Online play under a bad network: server, four game processes and one network simulator per player.

Usage: python tools/netsim_test.py [--cases NAME,NAME] [--port N] [--give-up S]

Every player reaches the server through its own tools/netsim.py, so delay, jitter and breaks hit
one player at a time. The game goes through the lobby into Free Play and plays the first minigame
(see lobby_test.py --game). A case passes when:
  delay cases:   all four play the minigame from the same step to the same step
  break cases:   the same, and the broken players connected again (reconnects > 0); after
                 restart_guest the new program's run counts for that player
  end cases:     the others end with exit code 4 and the expected sentence
Results go to run/netsim/<case>/ (logs of every process, simulator and the server).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lobby_test as lt  # noqa: E402
import net_minigame_matrix as matrix  # noqa: E402

OUT = os.path.join("run", "netsim")
NETSIM = os.path.join("tools", "netsim.py")
NAMES = ["host", "guest1", "guest2", "guest3"]

# name: one-way delay ms, jitter ms, loss share for every player; events after the minigame starts
CASES = {
    "plain": dict(net=(0, 0, 0)),
    "rtt50": dict(net=(25, 5, 0)),
    "rtt150": dict(net=(75, 15, 0)),
    "rtt300_loss": dict(net=(150, 25, 0.02)),
    "guest_cut": dict(net=(10, 2, 0), events=[(5, "cut", 2, 4)]),
    "guest_silent": dict(net=(10, 2, 0), events=[(5, "silent", 1, 10)]),
    "host_cut": dict(net=(10, 2, 0), events=[(5, "cut", 0, 4)]),
    "server_restart": dict(net=(10, 2, 0), events=[(5, "restart", None, 3)]),
    "guest_restart": dict(net=(10, 2, 0), events=[(5, "restart_guest", 2, 2)]),
    "guest_corrupt": dict(net=(10, 2, 0), events=[(5, "corrupt", 1, 0)]),
    "guest_gone": dict(net=(10, 2, 0), events=[(5, "kill", 3, 0)], expect_end="P4 (Guest3) ist nicht zurückgekommen"),
    "guest_leaves": dict(net=(10, 2, 0), leave=3, expect_end="P4 (Guest3) hat das Spiel verlassen"),
    "host_leaves": dict(net=(10, 2, 0), leave=0, expect_end="Der Host hat das Spiel beendet"),
}


class Run:
    def __init__(self, case, name, port, give_up):
        self.case = CASES[case]
        self.folder = os.path.join(OUT, case)
        shutil.rmtree(self.folder, ignore_errors=True)
        os.makedirs(self.folder)
        self.port = port
        self.give_up = give_up
        self.outputs = ["", "", "", ""]
        self.processes = []
        self.proxies = []
        self.server = None
        self.name = name

    def start_server(self):
        log = open(os.path.join(self.folder, "server.txt"), "a")
        # The same key across a restart, as a real server keeps its key file.
        self.server = subprocess.Popen([lt.SERVER, "--port", str(self.port), "--bind", "127.0.0.1",
                                        "--key", os.path.join(OUT, "server.key")],
                                       stdout=log, stderr=subprocess.STDOUT)
        time.sleep(0.3)

    def start_proxies(self):
        delay, jitter, loss = self.case["net"]
        for number in range(4):
            control = os.path.join(self.folder, f"control{number}.txt")
            self.proxies.append(subprocess.Popen(
                [sys.executable, NETSIM, "--listen", str(self.port + 1 + number), "--target", f"127.0.0.1:{self.port}",
                 "--delay", str(delay), "--jitter", str(jitter), "--loss", str(loss), "--seed", str(number + 1),
                 "--control", control, "--log", os.path.join(self.folder, f"netsim{number}.txt")]))
        time.sleep(0.5)

    def command(self, number, code_file, limit, rejoin=False):
        lobby = f"127.0.0.1:{self.port + 1 + number}"
        rejoin_file = os.path.join(self.folder, f"{NAMES[number]}.rejoin")
        common = ["--stop", "0xFFFFFFFC", "--max-blocks", limit, "--give-up", str(self.give_up),
                  "--end-after-minigame", "1"]
        common += ["--rejoin", rejoin_file] if rejoin else ["--lobby", lobby]
        if number != 0:
            common += ["--rejoin-file", rejoin_file]
        if self.case.get("leave") == number:
            common += ["--leave-at-tick", str(matrix.PLAY_TICK + 300 - (0 if number == 0 else matrix.GUEST_OFFSET))]
        if number == 0:
            args = [lt.EXE, lt.ROM, *common, "--name", "melonDS", "--save", matrix.SAVE, "--character", "Mario",
                    "--create", code_file, "--unlock-minigames", "73"]
            args += lt.host_menu_without_character()
            args += matrix.play_inputs(matrix.PLAY_TICK, 0)
            return args
        name, character = lt.GUESTS[number - 1]
        args = [lt.EXE, lt.ROM, *common, "--name", name, "--character", character, "--join", "@" + code_file,
                "--repeat", "400:800:50:touch;128;116", "--input", "2200:touch;223;178"]
        if rejoin:
            args = [a for a in args if a not in ("--join", "@" + code_file)]
        args += matrix.play_inputs(matrix.PLAY_TICK - matrix.GUEST_OFFSET, number)
        return args

    def read(self, number, process):
        for line in process.stdout:
            self.outputs[number] += line

    def run(self):
        code_file = os.path.join(self.folder, "code.txt")
        self.code_file = code_file
        self.start_server()
        self.start_proxies()
        play = matrix.PLAY_TICKS
        matrix.PLAY_TICKS = 3000
        limit = str(3 * (matrix.PLAY_TICK + 3000) * matrix.BLOCKS_PER_TICK)
        self.limit = limit
        readers = []
        self.readers = readers
        for number in range(4):
            process = subprocess.Popen(self.command(number, code_file, limit), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
            self.processes.append(process)
            reader = threading.Thread(target=self.read, args=(number, process), daemon=True)
            reader.start()
            readers.append(reader)
            time.sleep(0.5)
        matrix.PLAY_TICKS = play
        started = time.time()
        events = list(self.case.get("events", []))
        minigame_seen = None
        while any(p.poll() is None for p in self.processes):
            time.sleep(0.2)
            if minigame_seen is None and re.search(r"^scene 1 at", self.outputs[0], re.M):
                minigame_seen = time.time()
            if minigame_seen is not None:
                for event in list(events):
                    after, action, who, seconds = event
                    if time.time() - minigame_seen < after:
                        continue
                    events.remove(event)
                    self.act(action, who, seconds)
            if time.time() - started > 3600:
                for p in self.processes:
                    if p.poll() is None:
                        p.kill()
                self.outputs[0] += "\nABORT: test timeout\n"
        for reader in self.readers:
            reader.join(timeout=5)
        self.elapsed = time.time() - started
        for number, out in enumerate(self.outputs):
            with open(os.path.join(self.folder, f"{NAMES[number]}.txt"), "w", encoding="utf-8") as f:
                f.write(out)
        for proxy in self.proxies:
            proxy.kill()
        self.server.kill()

    def act(self, action, who, seconds):
        stamp = f"{time.strftime('%H:%M:%S')} event {action} {who} {seconds}"
        with open(os.path.join(self.folder, "events.txt"), "a") as f:
            f.write(stamp + "\n")
        print("  " + stamp)
        if action in ("cut", "silent", "corrupt"):
            with open(os.path.join(self.folder, f"control{who}.txt"), "w") as f:
                f.write(f"{action} {seconds}\n")
        elif action == "restart":
            self.server.kill()
            self.server.wait()
            time.sleep(seconds)
            self.start_server()
        elif action == "kill":
            self.processes[who].kill()
        elif action == "restart_guest":
            # The program ends without a word; a new one comes back from the file the old one wrote.
            self.processes[who].kill()
            self.processes[who].wait()
            time.sleep(0.5)
            with open(os.path.join(self.folder, f"{NAMES[who]}_before.txt"), "w", encoding="utf-8") as f:
                f.write(self.outputs[who])
            self.outputs[who] = ""
            time.sleep(seconds)
            play = matrix.PLAY_TICKS
            matrix.PLAY_TICKS = 3000
            process = subprocess.Popen(self.command(who, self.code_file, self.limit, rejoin=True),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                       encoding="utf-8", errors="replace")
            matrix.PLAY_TICKS = play
            self.processes[who] = process
            reader = threading.Thread(target=self.read, args=(who, process), daemon=True)
            reader.start()
            self.readers.append(reader)

    def judge(self):
        outputs = self.outputs
        missing = matrix.missing_lines(outputs)
        if missing:
            return None, missing
        notes = []
        failures = 0
        returncodes = [p.returncode for p in self.processes]
        stats = []
        for number, out in enumerate(outputs):
            status = re.findall(r"^net status: step (\d+), (.*), server (\d+) ms, reconnects (\d+), resent (\d+)", out, re.M)
            pings = [int(m) for s in status for m in re.findall(r"(\d+) ms", s[1])]
            last = status[-1] if status else None
            stats.append((pings, int(last[3]) if last else 0, int(last[4]) if last else 0))
        expect_end = self.case.get("expect_end")
        scenes = [int(s) for s in re.findall(r"^scene (\d+) at", outputs[0], re.M) if 1 <= int(s) <= 77]
        scene = scenes[0] if scenes else None
        delay = re.search(r"input delay (\d+)", outputs[1])
        notes.append(f"input delay {delay.group(1) if delay else '?'}, {self.elapsed:.0f} s")
        for number, out in enumerate(outputs):
            start, finish = matrix.scene_steps(out, scene) if scene else (None, None)
            pings, reconnects, resent = stats[number]
            ping_text = f"ping {min(pings)}..{max(pings)} ms" if pings else "no ping"
            over = re.search(r"^net: game over: (.*) \(step (\d+)\)", out, re.M)
            notes.append(f"{NAMES[number]}: exit {returncodes[number]}, minigame {start}..{finish}, {ping_text}, "
                         f"reconnects {reconnects}, resent {resent}" + (f", ended: {over.group(1)}" if over else ""))
        if expect_end is None:
            steps = {matrix.scene_steps(out, scene) if scene else (None, None) for out in outputs}
            if len(steps) != 1 or None in next(iter(steps)):
                failures += 1
                notes.append(f"FAIL: minigame steps differ: {steps}")
            if any("minigame ended" not in out for out in outputs):
                failures += 1
                notes.append("FAIL: not every player finished the minigame")
            for event in self.case.get("events", []):
                who = event[2]
                players = range(4) if who is None else [who]
                if event[1] == "restart_guest":
                    if "back with a new program" not in outputs[who]:
                        failures += 1
                        notes.append("FAIL: the new program did not come back")
                    continue
                if not any(stats[p][1] > 0 for p in players):
                    failures += 1
                    notes.append(f"FAIL: no reconnect after {event[1]}")
        else:
            leaver = self.case.get("leave")
            gone = [e[2] for e in self.case.get("events", []) if e[1] == "kill"]
            for number, out in enumerate(outputs):
                if number == leaver or number in gone:
                    continue
                if returncodes[number] != 4 or expect_end not in out:
                    failures += 1
                    notes.append(f"FAIL: {NAMES[number]} did not end with the expected sentence")
        return failures, notes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--port", type=int, default=47900)
    parser.add_argument("--give-up", type=int, default=20)
    args = parser.parse_args()
    failures = 0
    for index, case in enumerate(args.cases.split(",")):
        for attempt in range(1, 20):
            print(f"case {case}" + (f" (attempt {attempt})" if attempt > 1 else ""), flush=True)
            run = Run(case, case, args.port + 10 * index, args.give_up)
            run.run()
            result, notes = run.judge()
            if result is None:
                print(f"  missing code {sorted(notes)}")
                with open(matrix.ENTRIES, "a") as f:
                    for line in sorted(notes):
                        f.write(line + "\n")
                matrix.recompile()
                continue
            for note in notes:
                print("  " + note)
            print(f"  {'ok' if result == 0 else 'FAIL'} {case}", flush=True)
            failures += result
            break
    print("all passed" if failures == 0 else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
