#!/usr/bin/env python3
"""Print the game's mode and wireless state. Read-only; the game runs on.

Needs tools/gdb_server.py running. Usage: python tools/net_state.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import Session  # noqa: E402


def main():
    with Session() as gdb:
        gdb.interrupt()
        mode = gdb.read(0x020C9660, 16)
        print(f"mode {mode[0xF]:#x} next {mode[0xD]:#x} scene {struct.unpack('<h', mode[:2])[0]}")
        print(f"boot type {gdb.u16(0x027FFC40)}")
        flag = gdb.read(0x020D45A0 + 0x298, 1)[0]
        print(f"net flag byte [0x020D45A0+0x298] = {flag:#x} (bit 0 turns on net_state)")
        wh = gdb.read(0x020D1AE0, 0x40)
        print("WH block 0x020D1AE0:", wh.hex(" "))
        print(f"  +0x0E halfword {struct.unpack_from('<H', wh, 0xE)[0]}; "
              f"+0x1C state {struct.unpack_from('<I', wh, 0x1C)[0]}; "
              f"+0x34 {struct.unpack_from('<I', wh, 0x34)[0]}")
        shared = gdb.read(0x020D4700, 0xC0)
        for player in range(4):
            print(f"  shared[{player}]: {shared[player * 0x26:player * 0x26 + 8].hex(' ')}")
        print(f"  own record +0x98: {shared[0x98:0xA0].hex(' ')}  ready flag +0xBE {shared[0xBE]}")
        for player in range(4):
            base = 0x020CFCD8 + player * 0x38
            print(f"  player_pad[{player}] {base:#x}: {gdb.read(base, 12).hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
