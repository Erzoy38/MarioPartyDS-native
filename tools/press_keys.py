#!/usr/bin/env python3
"""Press DS buttons and touch the screen in the running game, without touching its logic.

The game's frame input routine (0x01FFC324) reads the hardware, then holds the pressed-key
mask in r9 and the touch sample at [sp] when it reaches 0x01FFC3B8. A breakpoint there
replaces those values for chosen frames, as if the buttons were really pressed.

Steps, comma separated:
  A, B, START, SELECT, UP, DOWN, LEFT, RIGHT, L, R, X, Y   press for 6 frames (A:20 for 20)
  touch=x;y or touch=x;y;frames                           touch the lower screen
  wait=seconds                                             let the game run

Prints the mode after every step. Needs tools/gdb_server.py running.
Usage: python tools/press_keys.py "touch=128;96,wait=2,A,wait=1"
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402

INPUT_READY = 0x01FFC3B8
MODE_STATE = 0x020C9660
BITS = {"A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
        "R": 8, "L": 9, "X": 10, "Y": 11}
R9 = 9
FRAME_TIMEOUT = 5.0


def mode_text(gdb):
    data = gdb.read(MODE_STATE, 16)
    return f"mode {data[0xF]:#x} next {data[0xD]:#x} scene {struct.unpack('<h', data[:2])[0]}"


def apply_frames(gdb, count, mask=None, touch=None):
    """Run count frames, changing the input at the breakpoint in each. The game is stopped."""
    for _ in range(count):
        gdb.resume()
        gdb.wait_stop(timeout=FRAME_TIMEOUT)
        gdb.drain(quiet=0.02)
        regs, _ = gdb.registers()
        if regs["pc"] != INPUT_READY:
            raise IOError(f"stopped at {regs['pc']:#x}, expected {INPUT_READY:#x}")
        if mask is not None:
            reply = gdb.command(f"P{R9:x}={struct.pack('<I', mask).hex()}")
            if reply != b"OK":
                raise IOError(f"register write failed: {reply!r}")
        if touch is not None:
            # [sp]: x, y, touched, validity as u16, read by the code that follows.
            gdb.write(regs["sp"], struct.pack("<HHHH", touch[0], touch[1], 1, 0))
        gdb.clear_break(INPUT_READY)
        gdb.step()
        gdb.drain(quiet=0.02)
        gdb.set_break(INPUT_READY)


def main():
    steps = sys.argv[1].split(",")
    with Session() as gdb:
        gdb.interrupt()
        print("start:", mode_text(gdb), flush=True)
        reply = gdb.set_break(INPUT_READY)
        if reply != b"OK":
            print("breakpoint:", reply)
            return 1
        try:
            for step in steps:
                step = step.strip()
                if step.startswith("wait="):
                    gdb.clear_break(INPUT_READY)
                    gdb.resume()
                    time.sleep(float(step[5:]))
                    gdb.interrupt()
                    gdb.set_break(INPUT_READY)
                elif step.startswith("touch="):
                    parts = [int(v) for v in step[6:].split(";")]
                    frames = parts[2] if len(parts) > 2 else 6
                    apply_frames(gdb, frames, touch=(parts[0], parts[1]))
                    apply_frames(gdb, 2)
                else:
                    name, _, frames = step.partition(":")
                    apply_frames(gdb, int(frames or 6), mask=1 << BITS[name.upper()])
                    apply_frames(gdb, 2, mask=0)
                print(f"{step:20} {mode_text(gdb)}", flush=True)
        except (socket.timeout, TimeoutError, IOError) as exc:
            print("FAILED:", exc, flush=True)
            return 1
        finally:
            gdb.clear_break(INPUT_READY)
    return 0


if __name__ == "__main__":
    sys.exit(main())
