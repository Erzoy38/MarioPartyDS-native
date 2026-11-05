#!/usr/bin/env python3
"""Self-test for instruction_set.py using hand-assembled encodings.

Usage: python tools/test_instruction_set.py
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import decode_arm, decode_thumb  # noqa: E402

BASE = 0x02000000

ARM = [
    (0xE3A00001, "mov r0,#1"),
    (0xE1A00000, "mov r0,r0"),
    (0xE1A00100, "mov r0,r0,lsl #2"),
    (0xE1A00211, "mov r0,r1,lsl r2"),
    (0xE0810002, "add r0,r1,r2"),
    (0xE2810C01, "add r0,r1,#0x100"),
    (0xE3500000, "cmp r0,#0"),
    (0x13A00001, "movne r0,#1"),
    (0xE59F1004, "ldr r1,[pc,#0x4]"),
    (0xE5912000, "ldr r2,[r1]"),
    (0xE5C12003, "strb r2,[r1,#0x3]"),
    (0xE4912004, "ldr r2,[r1],#0x4"),
    (0xE7912103, "ldr r2,[r1,r3,lsl #2]"),
    (0xE1D000B0, "ldrh r0,[r0]"),
    (0xE1C120B2, "strh r2,[r1,#0x2]"),
    (0xE1D100D1, "ldrsb r0,[r1,#0x1]"),
    (0xE1D100F2, "ldrsh r0,[r1,#0x2]"),
    (0xE1C020D0, "ldrd r2,[r0]"),
    (0xE1C020F0, "strd r2,[r0]"),
    (0xE0000291, "mul r0,r1,r2"),
    (0xE0203291, "mla r0,r1,r2,r3"),
    (0xE0810392, "umull r0,r1,r2,r3"),
    (0xE0C10392, "smull r0,r1,r2,r3"),
    (0xE1600281, "smulbb r0,r1,r2"),
    (0xE12002A1, "smulwb r0,r1,r2"),
    (0xE16F0F11, "clz r0,r1"),
    (0xE1020051, "qadd r0,r1,r2"),
    (0xE1020091, "swp r0,r1,[r2]"),
    (0xE12FFF1E, "bx lr"),
    (0xE12FFF11, "bx r1"),
    (0xE12FFF31, "blx r1"),
    (0xE92D4010, "push {r4,lr}"),
    (0xE8BD8010, "pop {r4,pc}"),
    (0xE8910003, "ldmia r1,{r0-r1}"),
    (0xE10F0000, "mrs r0,cpsr"),
    (0xE129F000, "msr cpsr_cf,r0"),
    (0xE321F0D3, "msr cpsr_c,#0xd3"),
    (0xEE070F90, "mcr p15,0,r0,c7,c0,4"),
    (0xEE110F10, "mrc p15,0,r0,c1,c0,0"),
    (0xEF000005, "swi #5"),
    (0xE1200070, "bkpt #0"),
    (0xEB000000, f"bl {BASE + 8:#010x}"),
    (0xEAFFFFFE, f"b {BASE:#010x}"),
    (0x0A000001, f"beq {BASE + 12:#010x}"),
    (0xFA000000, f"blx {BASE + 8:#010x}"),
    (0xFB000000, f"blx {BASE + 10:#010x}"),
]

THUMB = [
    (0x2001, None, "movs r0,#1"),
    (0x2800, None, "cmp r0,#0"),
    (0x1C40, None, "adds r0,r0,#1"),
    (0x1880, None, "adds r0,r0,r2"),
    (0x0048, None, "lsls r0,r1,#1"),
    (0x0008, None, "movs r0,r1"),
    (0x4008, None, "ands r0,r1"),
    (0x4348, None, "muls r0,r1"),
    (0x4288, None, "cmp r0,r1"),
    (0x4680, None, "mov r8,r0"),
    (0x46F7, None, "mov pc,lr"),
    (0x4770, None, "bx lr"),
    (0x4708, None, "bx r1"),
    (0x4788, None, "blx r1"),
    (0x4801, None, "ldr r0,[pc,#0x4]"),
    (0x5888, None, "ldr r0,[r1,r2]"),
    (0x6848, None, "ldr r0,[r1,#0x4]"),
    (0x7848, None, "ldrb r0,[r1,#0x1]"),
    (0x8848, None, "ldrh r0,[r1,#0x2]"),
    (0x9001, None, "str r0,[sp,#0x4]"),
    (0xA001, None, "add r0,pc,#0x4"),
    (0xA901, None, "add r1,sp,#0x4"),
    (0xB082, None, "sub sp,#0x8"),
    (0xB510, None, "push {r4,lr}"),
    (0xBD10, None, "pop {r4,pc}"),
    (0xC903, None, "ldmia r1!,{r0-r1}"),
    (0xDF05, None, "swi #5"),
    (0xD000, None, f"beq {BASE + 4:#010x}"),
    (0xE000, None, f"b {BASE + 4:#010x}"),
    (0xE7FE, None, f"b {BASE:#010x}"),
    (0xF000, 0xF800, f"bl {BASE + 4:#010x}"),
    (0xF000, 0xE800, f"blx {BASE + 4:#010x}"),
    (0xF7FF, 0xFFFE, f"bl {BASE:#010x}"),
]

FLOW = [
    ("arm bx lr returns", decode_arm(0xE12FFF1E, BASE).ret),
    ("arm bxne lr falls through", not decode_arm(0x112FFF1E, BASE).ends_flow),
    ("arm pop pc returns", decode_arm(0xE8BD8010, BASE).ret),
    ("arm bl keeps flowing", not decode_arm(0xEB000000, BASE).ends_flow),
    ("arm b ends flow", decode_arm(0xEA000000, BASE).ends_flow),
    ("arm literal address", decode_arm(0xE59F1004, BASE).literal == BASE + 12),
    ("arm adr address", decode_arm(0xE28F0010, BASE).ref == BASE + 0x18),
    ("arm blx switches to thumb", decode_arm(0xFA000000, BASE).target_thumb),
    ("thumb literal is word aligned", decode_thumb(0x4801, None, BASE + 2).literal == BASE + 8),
    ("thumb blx switches to arm", not decode_thumb(0xF000, 0xE800, BASE).target_thumb),
    ("thumb blx target is word aligned", decode_thumb(0xF000, 0xE800, BASE + 2).target == BASE + 4),
    ("thumb bl size is 4", decode_thumb(0xF000, 0xF800, BASE).size == 4),
    ("thumb pop pc returns", decode_thumb(0xBD10, None, BASE).ret),
    ("thumb lone bl prefix is undefined", decode_thumb(0xF000, 0x2001, BASE).undefined),
]


def main():
    failed = 0
    for word, expected in ARM:
        got = decode_arm(word, BASE).text
        if got != expected:
            print(f"ARM   {word:08x}: got {got!r}, expected {expected!r}")
            failed += 1
    for first, second, expected in THUMB:
        got = decode_thumb(first, second, BASE).text
        if got != expected:
            print(f"THUMB {first:04x}: got {got!r}, expected {expected!r}")
            failed += 1
    for name, passed in FLOW:
        if not passed:
            print(f"FLOW  {name}")
            failed += 1
    total = len(ARM) + len(THUMB) + len(FLOW)
    print(f"{total - failed}/{total} passed")

    # The decoder must never crash, whatever the bytes are.
    crashes = 0
    rng = random.Random(1234)
    for h in range(0x10000):
        for second in (None, 0xF800, 0xE801, 0x2001):
            try:
                decode_thumb(h, second, BASE).text
            except Exception as exc:
                crashes += 1
                print(f"crash thumb {h:04x}: {exc!r}")
    for _ in range(500000):
        word = rng.getrandbits(32)
        try:
            decode_arm(word, BASE).text
        except Exception as exc:
            crashes += 1
            print(f"crash arm {word:08x}: {exc!r}")
    print(f"fuzz crashes: {crashes}")
    failed += crashes
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
