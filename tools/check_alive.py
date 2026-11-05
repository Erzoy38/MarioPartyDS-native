#!/usr/bin/env python3
"""Does the game actually run between stops? Compares all of the game's work RAM.

Stops the game, reads the static BSS and overlay areas, lets it run, and compares. Bytes
that change while it runs prove it runs. No change suggests the game did not move, or
that nothing in RAM changes on this screen.

Needs tools/gdb_server.py running. Usage: python tools/check_alive.py [pause seconds]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402

AREAS = [("main ram", 0x020BDC80, 0x02400000 - 0x020BDC80), ("dtcm", 0x027E0000, 0x4000),
         ("oam", 0x07000000, 0x400)]


def snapshot(gdb):
    return {name: gdb.read(addr, size) for name, addr, size in AREAS}


def main():
    pause = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0
    with Session() as gdb:
        gdb.interrupt()
        before = snapshot(gdb)
        regs, _ = gdb.registers()
        print(f"first stop: pc={regs['pc']:#010x} lr={regs['lr']:#010x}")
        for round_number in range(3):
            gdb.resume()
            time.sleep(pause)
            gdb.interrupt()
            regs, _ = gdb.registers()
            after = snapshot(gdb)
            report = []
            for name, addr, _size in AREAS:
                changed = [i for i, (a, b) in enumerate(zip(before[name], after[name])) if a != b]
                first = f" first {addr + changed[0]:#010x}" if changed else ""
                report.append(f"{name}: {len(changed)} changed{first}")
            print(f"round {round_number}: pc={regs['pc']:#010x} " + "; ".join(report))
            before = after
    return 0


if __name__ == "__main__":
    sys.exit(main())
