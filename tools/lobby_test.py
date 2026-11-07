"""Tests the lobby server and the way from a room to a running game.

Usage: python tools/lobby_test.py [--rules] [--game] [--port N]

--rules  speaks the lobby protocol directly and checks every rule of the server: codes, joining,
         full room, taken characters, who may start, leaving, a host that leaves, expired codes,
         wrong version, bad names and broken messages. Also one refused join through the real client.
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
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_minigame_matrix as matrix  # noqa: E402

SERVER = os.path.join("native", "build", "MarioPartyDS-server.exe")
EXE = matrix.EXE
ROM = matrix.ROM
OUT = os.path.join("run", "lobbytest")

HELLO, CREATE, JOIN, PICK, READY, START, LEAVE, PING = 1, 2, 3, 4, 5, 6, 7, 8
ROOM, ERROR, STARTED, PONG, LEFT = 20, 21, 22, 23, 24
RELAY_FROM, RELAY_TO, RELAY_GONE = 30, 31, 32
ERRORS = {1: "bad version", 2: "bad name", 3: "unknown code", 4: "room full", 5: "already started",
          6: "character taken", 7: "not host", 8: "not enough players", 9: "not ready", 10: "expired",
          11: "host left", 12: "bad message", 13: "not in room", 14: "left room"}
CHARACTERS = ["Mario", "Luigi", "Wario", "Yoshi", "Peach", "Daisy", "Waluigi", "Toad"]


class Client:
    def __init__(self, port, name=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.inbox = b""
        if name is not None:
            self.send(HELLO, struct.pack("<H", 1) + bytes([len(name)]) + name.encode())

    def send(self, kind, body=b""):
        self.s.sendall(struct.pack("<I", len(body) + 1) + bytes([kind]) + body)

    def send_raw(self, data):
        self.s.sendall(data)

    def receive(self, wait=3.0):
        """Next message (type, body), or None after `wait` seconds."""
        end = time.time() + wait
        while True:
            if len(self.inbox) >= 5:
                length = struct.unpack_from("<I", self.inbox)[0]
                if len(self.inbox) >= 4 + length:
                    kind, body = self.inbox[4], self.inbox[5:4 + length]
                    self.inbox = self.inbox[4 + length:]
                    if kind == PONG:
                        continue
                    return kind, body
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                data = self.s.recv(65536)
            except socket.timeout:
                return None
            except OSError:
                return ("closed", b"")
            if not data:
                return ("closed", b"")
            self.inbox += data

    def expect(self, kind, wait=3.0):
        """Next message of `kind`; room updates before it are skipped (the lobby updates itself)."""
        got = self.receive(wait)
        while kind != ROOM and got is not None and got[0] == ROOM:
            got = self.receive(wait)
        if got is None or got[0] != kind:
            raise AssertionError(f"expected message {kind}, got {got}")
        return got[1]

    def expect_error(self, code):
        body = self.expect(ERROR)
        if body[0] != code:
            raise AssertionError(f"expected error {ERRORS[code]}, got {ERRORS.get(body[0], body[0])}")

    def latest_room(self, wait=0.3):
        room = None
        while True:
            got = self.receive(wait)
            if got is None:
                return room
            if got[0] == ROOM:
                room = parse_room(got[1])

    def close(self):
        self.s.close()


def parse_room(body):
    code = body[:6].decode()
    slot = body[6]
    at = 7
    seats = []
    for _ in range(4):
        present = body[at]
        n = body[at + 1]
        name = body[at + 2:at + 2 + n].decode()
        at += 2 + n
        character, ready = body[at], body[at + 1]
        ping = struct.unpack_from("<H", body, at + 2)[0]
        at += 4
        seats.append((name, CHARACTERS[character] if character < 8 else None, bool(ready), ping) if present else None)
    return {"code": code, "slot": slot, "seats": seats}


def start_server(port, *extra):
    os.makedirs(OUT, exist_ok=True)
    log = open(os.path.join(OUT, f"server_{port}.txt"), "w")
    server = subprocess.Popen([SERVER, "--port", str(port), "--bind", "127.0.0.1", *extra], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return server
        except OSError:
            time.sleep(0.1)
    raise SystemExit("server did not start")


def create_room(port, name="Host", character=0):
    host = Client(port, name)
    host.send(CREATE, bytes([character]))
    room = parse_room(host.expect(ROOM))
    return host, room["code"]


def join(port, code, name, character):
    guest = Client(port, name)
    guest.send(JOIN, code.encode() + bytes([character]))
    return guest


RULES = []


def rule(function):
    RULES.append(function)
    return function


@rule
def code_has_six_unambiguous_characters(port):
    host, code = create_room(port)
    assert len(code) == 6 and all(c in "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" for c in code), code
    codes = {code}
    for i in range(20):
        _, other = create_room(port, f"H{i}")
        codes.add(other)
    assert len(codes) == 21, "codes repeat"


@rule
def four_players_join_and_see_each_other(port):
    host, code = create_room(port, "Host", 0)
    guests = [join(port, code, f"G{i}", c) for i, c in ((1, 1), (2, 4), (3, 3))]
    for g in guests:
        g.expect(ROOM)
    room = host.latest_room()
    names = [s[0] for s in room["seats"]]
    assert names == ["Host", "G1", "G2", "G3"], names
    assert [s[1] for s in room["seats"]] == ["Mario", "Luigi", "Peach", "Yoshi"], room
    assert room["code"] == code


@rule
def lowercase_code_joins(port):
    host, code = create_room(port)
    guest = join(port, code.lower(), "G", 1)
    assert parse_room(guest.expect(ROOM))["code"] == code


@rule
def unknown_code_is_refused(port):
    guest = join(port, "ZZZZZ2", "G", 1)
    guest.expect_error(3)


@rule
def fifth_player_is_refused(port):
    host, code = create_room(port, "Host", 0)
    guests = [join(port, code, f"G{i}", i) for i in range(1, 4)]
    for guest in guests:
        guest.expect(ROOM)
    fifth = join(port, code, "G5", 5)
    fifth.expect_error(4)


@rule
def taken_character_is_refused_on_join_and_pick(port):
    host, code = create_room(port, "Host", 0)
    join(port, code, "G1", 0).expect_error(6)
    guest = join(port, code, "G2", 1)
    guest.expect(ROOM)
    guest.send(PICK, bytes([0]))
    guest.expect_error(6)
    guest.send(PICK, bytes([7]))
    room = guest.latest_room()
    assert room["seats"][1][1] == "Toad", room


@rule
def only_the_host_starts_and_only_when_ready(port):
    host, code = create_room(port, "Host", 0)
    host.send(START)
    host.expect_error(8)
    guest = join(port, code, "G1", 1)
    guest.expect(ROOM)
    host.latest_room()
    guest.send(START)
    guest.latest_room()
    host.send(START)
    host.expect_error(9)
    guest.send(READY, b"\x01")
    host.latest_room()
    guest.latest_room()
    host.send(START)
    body = host.expect(STARTED)
    assert body[0] == 0 and body[1] == 2 and body[2] == 0 and body[3] == 1, body
    body = guest.expect(STARTED)
    assert body[0] == 1 and body[1] == 2, body


@rule
def guest_start_is_refused(port):
    host, code = create_room(port, "Host", 0)
    guest = join(port, code, "G1", 1)
    guest.expect(ROOM)
    guest.send(START)
    while True:
        got = guest.receive()
        assert got is not None, "no answer"
        if got[0] == ERROR:
            assert got[1][0] == 7, got
            break


@rule
def leaving_updates_the_others(port):
    host, code = create_room(port, "Host", 0)
    g1 = join(port, code, "G1", 1)
    g1.expect(ROOM)
    g2 = join(port, code, "G2", 2)
    g2.expect(ROOM)
    host.latest_room()
    g1.send(LEAVE)
    assert g1.expect(LEFT)[0] == 14
    room = host.latest_room()
    assert room["seats"][1] is None and room["seats"][2][0] == "G2", room
    # a closed connection counts as leaving
    g2.close()
    room = host.latest_room(0.5)
    assert room["seats"][2] is None, room


@rule
def host_leaving_closes_the_room(port):
    host, code = create_room(port, "Host", 0)
    guest = join(port, code, "G1", 1)
    guest.expect(ROOM)
    host.send(LEAVE)
    while True:
        got = guest.receive()
        assert got is not None, "guest not told"
        if got[0] == LEFT:
            assert got[1][0] == 11, got
            break
    late = join(port, code, "G2", 2)
    late.expect_error(10)


@rule
def started_room_takes_nobody(port):
    host, code = create_room(port, "Host", 0)
    guest = join(port, code, "G1", 1)
    guest.expect(ROOM)
    guest.send(READY, b"\x01")
    host.latest_room()
    host.send(START)
    host.expect(STARTED)
    join(port, code, "G2", 2).expect_error(5)


@rule
def relay_passes_game_messages(port):
    host, code = create_room(port, "Host", 0)
    guest = join(port, code, "G1", 1)
    guest.expect(ROOM)
    guest.send(READY, b"\x01")
    host.latest_room()
    host.send(START)
    host.expect(STARTED)
    guest.expect(STARTED)
    guest.send(4, b"record")
    body = host.expect(RELAY_FROM)
    assert body == bytes([1, 4]) + b"record", body
    host.send(RELAY_TO, bytes([1, 5]) + b"step")
    got = guest.receive()
    assert got == (5, b"step"), got
    guest.close()
    body = host.expect(RELAY_GONE)
    assert body == bytes([1]), body


@rule
def wrong_version_and_names_are_refused(port):
    c = Client(port)
    c.send(HELLO, struct.pack("<H", 99) + b"\x01A")
    c.expect_error(1)
    c = Client(port)
    c.send(HELLO, struct.pack("<H", 1) + b"\x0bABCDEFGHIJK")
    c.expect_error(2)
    c = Client(port)
    c.send(HELLO, struct.pack("<H", 1) + b"\x00")
    c.expect_error(2)
    c = Client(port)
    c.send(CREATE, b"\x00")
    c.expect_error(12)


@rule
def broken_messages_are_refused(port):
    host, code = create_room(port, "Host", 0)
    host.send(PICK, b"\x09")  # no such character
    host.expect_error(12)
    host.send(PICK, b"")
    host.expect_error(12)
    host.send(99)
    host.expect_error(12)
    c = Client(port, "X")
    c.send_raw(struct.pack("<I", 0x7FFFFFFF) + b"\x01")
    got = c.receive()
    assert got == ("closed", b""), got
    # the server still works
    create_room(port, "After")


def expired_code(port):
    server = start_server(port, "--code-lifetime", "1")
    try:
        host, code = create_room(port, "Host", 0)
        guest = join(port, code, "G1", 1)
        guest.expect(ROOM)
        while True:
            got = host.receive(4)
            assert got is not None, "no expiry"
            if got[0] == LEFT:
                assert got[1][0] == 10, got
                break
        join(port, code, "G2", 2).expect_error(10)
    finally:
        server.kill()


def refused_through_real_client(port):
    result = subprocess.run([EXE, ROM, "--lobby", f"127.0.0.1:{port}", "--name", "Real", "--character", "Luigi",
                             "--join", "ABCDEF", "--lobby-exit", "1"], capture_output=True, text=True, timeout=60)
    assert result.returncode == 3, result.stdout
    assert "Diesen Code gibt es nicht" in result.stdout, result.stdout


def run_rules(port):
    server = start_server(port)
    failures = 0
    try:
        for function in RULES + [refused_through_real_client]:
            try:
                function(port)
                print(f"ok    {function.__name__}")
            except Exception as error:  # noqa: BLE001
                failures += 1
                print(f"FAIL  {function.__name__}: {error}")
    finally:
        server.kill()
    try:
        expired_code(port + 1)
        print("ok    expired_code")
    except Exception as error:  # noqa: BLE001
        failures += 1
        print(f"FAIL  expired_code: {error}")
    return failures


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
        failures += run_rules(args.port)
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
