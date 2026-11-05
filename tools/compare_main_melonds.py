#!/usr/bin/env python3
"""Stop melonDS at main after startup and compare it with the native run.

A reset ends the stub connection, so this needs melonDS's "break on startup" for the ARM9:
melonDS waits before the first instruction, tools/gdb_server.py --stay-stopped connects, and
this sets a breakpoint on main and lets the game run to it. Compares registers, CPSR and all
RAM areas with the dump from MarioPartyDS-headless.

Usage: python tools/compare_main_melonds.py <dump> [wait s]
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402

MAIN = 0x02003000
AREAS = [("main_ram", 0x02000000, 0x400000), ("itcm", 0x01FF8000, 0x8000),
         ("dtcm", 0x027E0000, 0x4000), ("palette", 0x05000000, 0x800), ("oam", 0x07000000, 0x800)]


def main():
    folder = sys.argv[1]
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 240.0
    with Session() as gdb:
        gdb.interrupt()
        print("breakpoint:", gdb.set_break(MAIN), flush=True)
        gdb.resume()
        print("running to main...", flush=True)
        end = time.time() + wait
        stop = None
        while time.time() < end and stop is None:
            try:
                stop = gdb.wait_stop(timeout=2.0)
            except (socket.timeout, TimeoutError):
                continue
        if stop is None:
            print("no stop within the waiting time")
            gdb.interrupt()
            gdb.clear_break(MAIN)
            return 2
        gdb.drain(quiet=0.1)
        gdb.halted_here = True
        regs, raw = gdb.registers()
        print("stop:", stop, f"pc={regs['pc']:#010x}")
        print("melonDS:", " ".join(f"{k}={v:08x}" for k, v in regs.items()))
        words = [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little") for i in range(0, len(raw), 8)]
        print(f"melonDS cpsr={words[16]:08x}")
        failures = 0
        for name, base, size in AREAS:
            live = gdb.read(base, size)
            with open(os.path.join(folder, f"{name}.bin"), "rb") as f:
                native = f.read()
            count = min(len(native), size)
            diffs = [i for i in range(count) if live[i] != native[i]]
            print(f"{name:9} {count - len(diffs)}/{count} bytes equal")
            if diffs:
                failures += 1
                runs = []
                for i in diffs:
                    if runs and i - runs[-1][1] <= 16:
                        runs[-1][1] = i
                    else:
                        runs.append([i, i])
                for s, e in runs[:14]:
                    print(f"    {base + s:#010x}-{base + e + 1:#010x} melonDS "
                          f"{live[s:min(e + 1, s + 12)].hex(' ')} native {native[s:min(e + 1, s + 12)].hex(' ')}")
                print(f"    {len(runs)} differing ranges")
        gdb.clear_break(MAIN)
    print("done", "(differences above)" if failures else "(all equal)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
