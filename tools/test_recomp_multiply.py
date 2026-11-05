"""Checks which registers the recompiler uses for the halfword multiplies.

These put the result in bits 19-16, unlike most ARM instructions. Getting that wrong sent the
game's scene dispatch at 0x0201D0D0 to a bogus address, so the cases below come from that code.
"""
import sys

sys.path.insert(0, "tools")
import recomp


def text(word):
    return " ".join(recomp.arm_group0(word, 0x02000000))


def expect(name, word, *parts):
    code = text(word)
    missing = [part for part in parts if part not in code]
    if missing:
        print(f"FAILED {name}: {missing} not in {code}")
        return 1
    return 0


failed = 0
# smlabb r0,r5,r0,r1 (the instruction from the game: r0 = r5 * r0 + r1)
failed += expect("smlabb", 0xE1001085, "s32(c.r[1])", "c.r[0] = u32(sum)")
# smlawb r2,r3,r4,r5: r2 = (r3 * r4) >> 16 + r5
failed += expect("smlawb", 0xE1225483, "s32(c.r[5])", "c.r[2] = u32(sum)")
# smulwb r2,r3,r4
failed += expect("smulwb", 0xE12204A3, "c.r[2] = u32(s32(")
# smlalbb r2,r3,r4,r5: low word r2, high word r3
failed += expect("smlalbb", 0xE1432584, "(u64(c.r[3]) << 32) | c.r[2]", "c.r[2] = u32(pair)",
                 "c.r[3] = u32(pair >> 32)")
# smulbb r2,r3,r4
failed += expect("smulbb", 0xE1620483, "c.r[2] = u32(")

print("FAILED" if failed else "ok")
sys.exit(1 if failed else 0)
