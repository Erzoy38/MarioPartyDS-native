#!/usr/bin/env python3
"""Static recompiler: translates the game's ARM and Thumb code to C++ blocks.

Finds code with the tracer from known entry points, splits it into basic blocks and writes
one C++ function per block to native/generated/. That folder holds translated game code, so
it is never committed; run this on a machine that has the player's own unpacked ROM.

Usage: python tools/recomp.py [--entries file] [--per-file N]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import sext  # noqa: E402
from nds_image import ARM9_BASE, Space  # noqa: E402
from trace_code import Tracer  # noqa: E402

OUT = os.path.join("native", "generated")
# Mode entry functions found in the program table (decode_modes.py), and the start.
DEFAULT_ENTRIES = [(ARM9_BASE + 0x800, False)] + [(a, False) for a in (
    0x02008120, 0x02054288, 0x020084BC, 0x02008114, 0x0201A704, 0x0201A720, 0x0201A7F0,
    0x0201A834, 0x0202C7A8, 0x0202FBFC, 0x02029DC0)]


def reg(n, pc_value):
    return f"0x{pc_value:X}u" if n == 15 else f"c.r[{n}]"


def op2_arm(w, addr):
    """Statements defining u32 op2 and u32 sc (shifter carry out)."""
    if w >> 25 & 1:
        rotate = ((w >> 8) & 15) * 2
        imm = w & 0xFF
        value = ((imm >> rotate) | (imm << (32 - rotate))) & 0xFFFFFFFF if rotate else imm
        carry = f"{value >> 31}u" if rotate else "c.cf"
        return f"const u32 op2 = 0x{value:X}u; const u32 sc = {carry};"
    rm = w & 15
    kind = (w >> 5) & 3
    if w >> 4 & 1:
        rs = (w >> 8) & 15
        value = reg(rm, addr + 12)
        amount = f"c.r[{rs}]"
        return f"u32 sc; const u32 op2 = shift_reg({kind}, {value}, {amount}, sc, c.cf);"
    amount = (w >> 7) & 31
    return f"u32 sc; const u32 op2 = shift_imm({kind}, {reg(rm, addr + 8)}, {amount}, sc, c.cf);"


def arm_data_processing(w, addr):
    opcode = (w >> 21) & 15
    s = w >> 20 & 1
    rn = (w >> 16) & 15
    rd = (w >> 12) & 15
    a = reg(rn, addr + 8 if (w >> 25 & 1) or not (w >> 4 & 1) else addr + 12)
    out = [op2_arm(w, addr)]
    target = "pc_value" if rd == 15 else f"c.r[{rd}]"
    declare = "const u32 pc_value = " if rd == 15 else f"{target} = "
    flags = s and rd != 15

    def logical(expr, write=True):
        if write:
            if flags:
                out.append(f"const u32 res = {expr}; set_nz(c, res); c.cf = sc; {declare}res;")
            else:
                out.append(f"{declare}{expr};")
        else:
            out.append(f"const u32 res = {expr}; set_nz(c, res); c.cf = sc;")

    def arith(x, y, carry, write=True):
        if flags or not write:
            line = f"const u32 res = add_flags(c, {x}, {y}, {carry});"
            out.append(line + (f" {declare}res;" if write else ""))
        else:
            out.append(f"{declare}{x} + {y} + {carry};")

    if opcode == 0: logical(f"{a} & op2")
    elif opcode == 1: logical(f"{a} ^ op2")
    elif opcode == 2: arith(a, "~op2", "1u")
    elif opcode == 3: arith("op2", f"~{a}", "1u")
    elif opcode == 4: arith(a, "op2", "0u")
    elif opcode == 5: arith(a, "op2", "c.cf")
    elif opcode == 6: arith(a, "~op2", "c.cf")
    elif opcode == 7: arith("op2", f"~{a}", "c.cf")
    elif opcode == 8: logical(f"{a} & op2", write=False)
    elif opcode == 9: logical(f"{a} ^ op2", write=False)
    elif opcode == 10: arith(a, "~op2", "1u", write=False)
    elif opcode == 11: arith(a, "op2", "0u", write=False)
    elif opcode == 12: logical(f"{a} | op2")
    elif opcode == 13: logical("op2")
    elif opcode == 14: logical(f"{a} & ~op2")
    else: logical("~op2")
    if rd == 15:
        if s:
            out.append("c.exception_return(pc_value); return;")
        else:
            out.append("c.pc = pc_value & ~3u; return;")
    return out


def arm_load_store(w, addr):
    is_reg, p, u, byte, wb, load = (w >> 25 & 1, w >> 24 & 1, w >> 23 & 1, w >> 22 & 1,
                                    w >> 21 & 1, w >> 20 & 1)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    out = []
    if is_reg:
        out.append(f"u32 unused_carry; const u32 offset = shift_imm({(w >> 5) & 3}, "
                   f"{reg(w & 15, addr + 8)}, {(w >> 7) & 31}, unused_carry, c.cf);")
    else:
        out.append(f"const u32 offset = 0x{w & 0xFFF:X}u;")
    sign = "+" if u else "-"
    base = reg(rn, addr + 8)
    out.append(f"const u32 base = {base};")
    out.append(f"const u32 where = base{f' {sign} offset' if p else ''};")
    if load:
        out.append("const u32 value = " + ("rd8(where)" if byte else "ldr_rotated(where)") + ";")
    else:
        source = reg(rd, addr + 12)
        out.append(f"wr8(where, u8({source}));" if byte else f"wr32(where, {source});")
    if not p:
        out.append(f"c.r[{rn}] = base {sign} offset;")
    elif wb:
        out.append(f"c.r[{rn}] = where;")
    if load:
        if rd == 15:
            out.append("c.branch_exchange(value); return;")
        else:
            out.append(f"c.r[{rd}] = value;")
    return out


def arm_extra_load_store(w, addr):
    p, u, imm, wb, load = (w >> 24 & 1, w >> 23 & 1, w >> 22 & 1, w >> 21 & 1, w >> 20 & 1)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    op = (w >> 5) & 3
    offset = f"0x{((w >> 4) & 0xF0) | (w & 15):X}u" if imm else reg(w & 15, addr + 8)
    sign = "+" if u else "-"
    out = [f"const u32 base = {reg(rn, addr + 8)};",
           f"const u32 where = base{f' {sign} {offset}' if p else ''};"]
    after = []
    if not p:
        after.append(f"c.r[{rn}] = base {sign} {offset};")
    elif wb:
        after.append(f"c.r[{rn}] = where;")
    if load:
        expr = {1: "rd16(where)", 2: "u32(s32(s8(rd8(where))))", 3: "u32(s32(s16(rd16(where))))"}[op]
        return out + [f"const u32 value = {expr};"] + after + [f"c.r[{rd}] = value;"]
    if op == 1:
        return out + [f"wr16(where, u16({reg(rd, addr + 12)}));"] + after
    if op == 2:  # ldrd
        return out + ["const u32 lo = rd32(where), hi = rd32(where + 4);"] + after + [
            f"c.r[{rd}] = lo; c.r[{rd + 1}] = hi;"]
    return out + [f"wr32(where, c.r[{rd}]); wr32(where + 4, c.r[{rd + 1}]);"] + after


def arm_block_transfer(w, addr):
    p, u, s, wb, load = (w >> 24 & 1, w >> 23 & 1, w >> 22 & 1, w >> 21 & 1, w >> 20 & 1)
    rn = (w >> 16) & 15
    mask = w & 0xFFFF
    count = bin(mask).count("1")
    regs = [i for i in range(16) if mask >> i & 1]
    size = count * 4
    out = [f"const u32 base = c.r[{rn}];"]
    start = {(1, 0): "base", (1, 1): "base + 4", (0, 0): f"base - {size - 4}", (0, 1): f"base - {size}"}[(u, p)]
    out.append(f"u32 where = ({start}) & ~3u;")
    final = f"base + {size}" if u else f"base - {size}"
    user = s and not (load and mask >> 15 & 1)
    if wb and not load:
        pass
    writeback = f"c.r[{rn}] = {final};" if wb else ""
    if load:
        if writeback and not (mask >> rn & 1):
            out.append(writeback)
        for i in regs:
            if i == 15:
                continue
            setter = f"c.set_user_reg({i}, rd32(where));" if user else f"c.r[{i}] = rd32(where);"
            out.append(f"{setter} where += 4;")
        if mask >> 15 & 1:
            out.append("const u32 target = rd32(where);")
            out.append("c.exception_return(target); return;" if s else "c.branch_exchange(target); return;")
    else:
        for i in regs:
            value = f"0x{addr + 12:X}u" if i == 15 else (f"c.user_reg({i})" if user else f"c.r[{i}]")
            out.append(f"wr32(where, {value}); where += 4;")
        if writeback:
            out.append(writeback)
    return out


def arm_group0(w, addr):
    rd, rn, rs, rm = (w >> 12) & 15, (w >> 16) & 15, (w >> 8) & 15, w & 15
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        return [f"c.branch_exchange({reg(rm, addr + 8)}); return;"]
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        return [f"const u32 target = {reg(rm, addr + 8)}; c.r[14] = 0x{addr + 4:X}u;",
                "c.branch_exchange(target); return;"]
    if (w & 0x0FFF0FF0) == 0x016F0F10:
        return [f"c.r[{rd}] = count_leading_zeros(c.r[{rm}]);"]
    if (w & 0xFFF000F0) == 0xE1200070:
        return [f'runtime_abort("bkpt", 0x{addr:X}u);']
    if (w & 0x0F900FF0) == 0x01000050:
        op = (w >> 21) & 3
        x, y = f"s64(s32(c.r[{rm}]))", f"s64(s32(c.r[{rn}]))"
        if op == 2:
            y = f"s64(saturate({y} * 2, c.q))"
        if op == 3:
            return [f"c.r[{rd}] = u32(saturate({x} - s64(saturate({y} * 2, c.q)), c.q));"]
        return [f"c.r[{rd}] = u32(saturate({x} {'-' if op == 1 else '+'} {y}, c.q));"]
    if (w & 0x0F900090) == 0x01000080:
        op = (w >> 21) & 3
        x_top, y_top = w >> 5 & 1, w >> 6 & 1
        x_half = f"s32(s16(c.r[{rm}] >> {16 if x_top else 0}))"
        y_half = f"s32(s16(c.r[{rs}] >> {16 if y_top else 0}))"
        dest, acc = rn, rd
        if op == 0:
            return [f"const s64 sum = s64({x_half} * {y_half}) + s64(s32(c.r[{dest}]));",
                    f"if (sum != s32(sum)) c.q = 1; c.r[{acc}] = u32(sum);"]
        if op == 1:
            product = f"((s64(s32(c.r[{rm}])) * {y_half}) >> 16)"
            if x_top:
                return [f"c.r[{acc}] = u32(s32({product}));"]
            return [f"const s64 sum = {product} + s64(s32(c.r[{dest}]));",
                    f"if (sum != s32(sum)) c.q = 1; c.r[{acc}] = u32(sum);"]
        if op == 2:
            return [f"u64 pair = (u64(c.r[{acc}]) << 32) | c.r[{dest}];",
                    f"pair += u64(s64({x_half} * {y_half}));",
                    f"c.r[{dest}] = u32(pair); c.r[{acc}] = u32(pair >> 32);"]
        return [f"c.r[{acc}] = u32({x_half} * {y_half});"]
    if (w & 0x0FC000F0) == 0x00000090:
        s = w >> 20 & 1
        expr = f"c.r[{rm}] * c.r[{rs}]"
        if w >> 21 & 1:
            expr += f" + c.r[{rd}]"
        return [f"const u32 res = {expr}; c.r[{rn}] = res;" + (" set_nz(c, res);" if s else "")]
    if (w & 0x0F8000F0) == 0x00800090:
        signed, acc, s = w >> 22 & 1, w >> 21 & 1, w >> 20 & 1
        cast = "s64(s32({}))" if signed else "u64({})"
        mul = f"u64({cast.format(f'c.r[{rm}]')} * {cast.format(f'c.r[{rs}]')})"
        out = [f"u64 res = {mul};"]
        if acc:
            out.append(f"res += (u64(c.r[{rn}]) << 32) | c.r[{rd}];")
        out.append(f"c.r[{rd}] = u32(res); c.r[{rn}] = u32(res >> 32);")
        if s:
            out.append("c.n = u32(res >> 63); c.z = res == 0;")
        return out
    if (w & 0x0FB00FF0) == 0x01000090:
        if w >> 22 & 1:
            return [f"const u32 base = c.r[{rn}]; const u32 old = rd8(base);",
                    f"wr8(base, u8(c.r[{rm}])); c.r[{rd}] = old;"]
        return [f"const u32 base = c.r[{rn}]; const u32 old = ldr_rotated(base);",
                f"wr32(base, c.r[{rm}]); c.r[{rd}] = old;"]
    if (w & 0x0E000090) == 0x00000090 and (w >> 5) & 3:
        return arm_extra_load_store(w, addr)
    if (w & 0x0FBF0FFF) == 0x010F0000:
        return [f"c.r[{rd}] = {'c.spsr' if w >> 22 & 1 else 'c.cpsr()'};"]
    if (w & 0x0FB0FFF0) == 0x0120F000 or (w & 0x0FB0F000) == 0x0320F000:
        mask = 0
        for bit, field in zip(range(16, 20), (0xFF, 0xFF00, 0xFF0000, 0xFF000000)):
            if w >> bit & 1:
                mask |= field
        if w >> 25 & 1:
            rotate = ((w >> 8) & 15) * 2
            imm = w & 0xFF
            value = ((imm >> rotate) | (imm << (32 - rotate))) & 0xFFFFFFFF if rotate else imm
            source = f"0x{value:X}u"
        else:
            source = f"c.r[{rm}]"
        if w >> 22 & 1:
            return [f"c.spsr = (c.spsr & ~0x{mask:X}u) | ({source} & 0x{mask:X}u);"]
        return [f"c.set_cpsr({source}, 0x{mask:X}u);"]
    return arm_data_processing(w, addr)


def arm_statements(w, addr):
    top = (w >> 26) & 3
    if w >> 28 == 15:
        if (w & 0xFE000000) == 0xFA000000:
            target = (addr + 8 + (sext(w & 0xFFFFFF, 24) << 2) + ((w >> 23) & 2)) & 0xFFFFFFFF
            return [f"c.r[14] = 0x{addr + 4:X}u; c.thumb = 1; c.pc = 0x{target:X}u; return;"]
        if (w & 0xFD70F000) == 0xF550F000:
            return []  # pld
        return [f'runtime_abort("unhandled unconditional instruction", 0x{addr:X}u);']
    if top == 0:
        return arm_group0(w, addr)
    if top == 1:
        if w >> 25 & 1 and w >> 4 & 1:
            return [f'runtime_abort("undefined instruction", 0x{addr:X}u);']
        return arm_load_store(w, addr)
    kind = (w >> 25) & 7
    if kind == 4:
        return arm_block_transfer(w, addr)
    if kind == 5:
        target = (addr + 8 + (sext(w & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
        link = f"c.r[14] = 0x{addr + 4:X}u; " if w >> 24 & 1 else ""
        return [f"{link}c.pc = 0x{target:X}u; return;"]
    if (w >> 24) & 15 == 15:
        return [f"c.pc = 0x{addr + 4:X}u; software_interrupt(c, 0x{(w & 0xFFFFFF) >> 16:X}u);"]
    if (w >> 24) & 15 == 14 and w >> 4 & 1 and (w >> 8) & 15 == 15:
        op1, crn, reg_d, op2, crm = (w >> 21) & 7, (w >> 16) & 15, (w >> 12) & 15, (w >> 5) & 7, w & 15
        if w >> 20 & 1:
            return [f"c.r[{reg_d}] = cp15_read(c, {op1}, {crn}, {crm}, {op2});"]
        return [f"cp15_write(c, {op1}, {crn}, {crm}, {op2}, c.r[{reg_d}]);"]
    return [f'runtime_abort("unhandled coprocessor instruction", 0x{addr:X}u);']


ALU = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg", "cmp", "cmn",
       "orr", "mul", "bic", "mvn"]


def thumb_alu(op, rd, rs):
    d, s = f"c.r[{rd}]", f"c.r[{rs}]"
    if op == 0: return [f"{d} &= {s}; set_nz(c, {d});"]
    if op == 1: return [f"{d} ^= {s}; set_nz(c, {d});"]
    if op in (2, 3, 4, 7):
        kind = {2: 0, 3: 1, 4: 2, 7: 3}[op]
        return [f"u32 sc; {d} = shift_reg({kind}, {d}, {s}, sc, c.cf); c.cf = sc; set_nz(c, {d});"]
    if op == 5: return [f"{d} = add_flags(c, {d}, {s}, c.cf);"]
    if op == 6: return [f"{d} = add_flags(c, {d}, ~{s}, c.cf);"]
    if op == 8: return [f"set_nz(c, {d} & {s});"]
    if op == 9: return [f"{d} = add_flags(c, 0u, ~{s}, 1u);"]
    if op == 10: return [f"add_flags(c, {d}, ~{s}, 1u);"]
    if op == 11: return [f"add_flags(c, {d}, {s}, 0u);"]
    if op == 12: return [f"{d} |= {s}; set_nz(c, {d});"]
    if op == 13: return [f"{d} *= {s}; set_nz(c, {d});"]
    if op == 14: return [f"{d} &= ~{s}; set_nz(c, {d});"]
    return [f"{d} = ~{s}; set_nz(c, {d});"]


def thumb_statements(h, next_h, addr):
    pc = addr + 4
    rd3, rs3 = h & 7, (h >> 3) & 7
    top3 = h >> 13
    if top3 == 0:
        op = (h >> 11) & 3
        if op < 3:
            amount = (h >> 6) & 31
            return [f"u32 sc; c.r[{rd3}] = shift_imm({op}, c.r[{rs3}], {amount}, sc, c.cf); "
                    f"c.cf = sc; set_nz(c, c.r[{rd3}]);"]
        field = (h >> 6) & 7
        value = f"{field}u" if h >> 10 & 1 else f"c.r[{field}]"
        if h >> 9 & 1:
            return [f"c.r[{rd3}] = add_flags(c, c.r[{rs3}], ~{value}, 1u);"]
        return [f"c.r[{rd3}] = add_flags(c, c.r[{rs3}], {value}, 0u);"]
    if top3 == 1:
        op, rd, imm = (h >> 11) & 3, (h >> 8) & 7, h & 0xFF
        if op == 0: return [f"c.r[{rd}] = {imm}u; set_nz(c, {imm}u);"]
        if op == 1: return [f"add_flags(c, c.r[{rd}], ~{imm}u, 1u);"]
        if op == 2: return [f"c.r[{rd}] = add_flags(c, c.r[{rd}], {imm}u, 0u);"]
        return [f"c.r[{rd}] = add_flags(c, c.r[{rd}], ~{imm}u, 1u);"]
    if h >> 10 == 0b010000:
        return thumb_alu((h >> 6) & 15, rd3, rs3)
    if h >> 10 == 0b010001:
        op = (h >> 8) & 3
        rd = (h & 7) | ((h >> 4) & 8)
        rm = (h >> 3) & 15
        value = reg(rm, pc)
        if op == 3:
            if h >> 7 & 1:
                return [f"const u32 target = {value}; c.r[14] = 0x{addr + 2:X}u | 1u;",
                        "c.branch_exchange(target); return;"]
            return [f"c.branch_exchange({value}); return;"]
        if op == 1:
            return [f"add_flags(c, {reg(rd, pc)}, ~{value}, 1u);"]
        if rd == 15:
            expr = f"0x{pc:X}u + {value}" if op == 0 else value
            return [f"c.pc = ({expr}) & ~1u; return;"]
        return [f"c.r[{rd}] = c.r[{rd}] + {value};" if op == 0 else f"c.r[{rd}] = {value};"]
    if h >> 11 == 0b01001:
        rd = (h >> 8) & 7
        return [f"c.r[{rd}] = rd32(0x{(pc & ~3) + (h & 0xFF) * 4:X}u);"]
    if h >> 12 == 0b0101:
        op = (h >> 9) & 7
        where = f"c.r[{rs3}] + c.r[{(h >> 6) & 7}]"
        out = [f"const u32 where = {where};"]
        if op == 0: out.append(f"wr32(where, c.r[{rd3}]);")
        elif op == 1: out.append(f"wr16(where, u16(c.r[{rd3}]));")
        elif op == 2: out.append(f"wr8(where, u8(c.r[{rd3}]));")
        elif op == 3: out.append(f"c.r[{rd3}] = u32(s32(s8(rd8(where))));")
        elif op == 4: out.append(f"c.r[{rd3}] = ldr_rotated(where);")
        elif op == 5: out.append(f"c.r[{rd3}] = rd16(where);")
        elif op == 6: out.append(f"c.r[{rd3}] = rd8(where);")
        else: out.append(f"c.r[{rd3}] = u32(s32(s16(rd16(where))));")
        return out
    if top3 == 0b011:
        byte, load = h >> 12 & 1, h >> 11 & 1
        offset = ((h >> 6) & 31) * (1 if byte else 4)
        where = f"c.r[{rs3}] + {offset}u"
        if load:
            return [f"c.r[{rd3}] = {'rd8' if byte else 'ldr_rotated'}({where});"]
        return [f"wr8({where}, u8(c.r[{rd3}]));" if byte else f"wr32({where}, c.r[{rd3}]);"]
    top4 = h >> 12
    if top4 == 0b1000:
        where = f"c.r[{rs3}] + {((h >> 6) & 31) * 2}u"
        if h >> 11 & 1:
            return [f"c.r[{rd3}] = rd16({where});"]
        return [f"wr16({where}, u16(c.r[{rd3}]));"]
    if top4 == 0b1001:
        rd = (h >> 8) & 7
        where = f"c.r[13] + {(h & 0xFF) * 4}u"
        return [f"c.r[{rd}] = ldr_rotated({where});" if h >> 11 & 1 else f"wr32({where}, c.r[{rd}]);"]
    if top4 == 0b1010:
        rd = (h >> 8) & 7
        offset = (h & 0xFF) * 4
        if h >> 11 & 1:
            return [f"c.r[{rd}] = c.r[13] + {offset}u;"]
        return [f"c.r[{rd}] = 0x{(pc & ~3) + offset:X}u;"]
    if top4 == 0b1011:
        if h >> 8 == 0xB0:
            sign = "-" if h >> 7 & 1 else "+"
            return [f"c.r[13] = c.r[13] {sign} {(h & 0x7F) * 4}u;"]
        if (h >> 9) & 3 == 2:
            load, extra = h >> 11 & 1, h >> 8 & 1
            regs = [i for i in range(8) if h >> i & 1]
            if load:
                out = ["u32 where = c.r[13];"]
                for i in regs:
                    out.append(f"c.r[{i}] = rd32(where); where += 4;")
                if extra:
                    out.append("const u32 target = rd32(where); where += 4; c.r[13] = where;")
                    out.append("c.branch_exchange(target); return;")
                else:
                    out.append("c.r[13] = where;")
                return out
            size = (len(regs) + extra) * 4
            out = [f"u32 where = c.r[13] - {size}u; c.r[13] = where;"]
            for i in regs:
                out.append(f"wr32(where, c.r[{i}]); where += 4;")
            if extra:
                out.append("wr32(where, c.r[14]);")
            return out
        return [f'runtime_abort("unhandled Thumb instruction", 0x{addr:X}u);']
    if top4 == 0b1100:
        rb = (h >> 8) & 7
        regs = [i for i in range(8) if h >> i & 1]
        out = [f"u32 where = c.r[{rb}];"]
        if h >> 11 & 1:
            for i in regs:
                out.append(f"c.r[{i}] = rd32(where); where += 4;")
            if rb not in regs:
                out.append(f"c.r[{rb}] = where;")
        else:
            for i in regs:
                out.append(f"wr32(where, c.r[{i}]); where += 4;")
            out.append(f"c.r[{rb}] = where;")
        return out
    if top4 == 0b1101:
        code = (h >> 8) & 15
        if code == 15:
            return [f"c.pc = 0x{addr + 2:X}u; software_interrupt(c, {h & 0xFF}u);"]
        target = (pc + (sext(h & 0xFF, 8) << 1)) & 0xFFFFFFFF
        return [f"if (cond(c, {code})) {{ c.pc = 0x{target:X}u; return; }}"]
    five = h >> 11
    if five == 0b11100:
        target = (pc + (sext(h & 0x7FF, 11) << 1)) & 0xFFFFFFFF
        return [f"c.pc = 0x{target:X}u; return;"]
    if five == 0b11110 and next_h is not None:
        offset = (sext(h & 0x7FF, 11) << 12) + ((next_h & 0x7FF) << 1)
        target = (pc + offset) & 0xFFFFFFFF
        back = f"c.r[14] = 0x{addr + 4:X}u | 1u;"
        if next_h >> 11 == 0b11111:
            return [f"{back} c.pc = 0x{target:X}u; return;"]
        return [f"{back} c.thumb = 0; c.pc = 0x{target & ~3:X}u; return;"]
    return [f'runtime_abort("unhandled Thumb instruction", 0x{addr:X}u);']


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--per-file", type=int, default=4000)
    args = ap.parse_args()

    space = Space()
    tracer = Tracer(space, list(space.segments))
    for addr, thumb in DEFAULT_ENTRIES:
        tracer.add(addr, thumb, "entry")
    tracer.run()
    seen, modes = tracer.seen, tracer.mode_at
    print(f"instructions: {len(seen)}, undefined reached: {len(tracer.undefined)}")

    leaders = set()
    for addr, thumb in DEFAULT_ENTRIES:
        leaders.add(addr)
    for addr, insn in seen.items():
        if insn.target is not None:
            leaders.add(insn.target)
        if insn.target is not None or insn.ret or insn.indirect or insn.call or insn.jump:
            leaders.add(addr + insn.size)
    # Addresses a resolved indirect branch reaches.
    for (addr, _thumb) in tracer.functions:
        leaders.add(addr)
    leaders &= set(seen)

    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        os.remove(os.path.join(OUT, name))

    def statements(addr, insn):
        if modes[addr]:
            raw = insn.raw
            h = raw >> 16 if insn.size == 4 else raw
            next_h = raw & 0xFFFF if insn.size == 4 else None
            return thumb_statements(h, next_h, addr)
        if insn.undefined:
            return [f'runtime_abort("undefined instruction", 0x{addr:X}u);']
        return arm_statements(insn.raw, addr)

    blocks = []
    ordered = sorted(leaders)
    leader_set = set(ordered)
    for start in ordered:
        lines = []
        addr = start
        while True:
            insn = seen[addr]
            body = statements(addr, insn)
            text = " ".join(body)
            condition = None
            if not modes[addr] and insn.raw >> 28 not in (14, 15):
                condition = insn.raw >> 28
            comment = insn.text.replace("*/", "")
            if condition is not None:
                lines.append(f"  if (cond(c, {condition})) {{ {text} }}  // {addr:08x} {comment}")
            else:
                lines.append(f"  {{ {text} }}  // {addr:08x} {comment}")
            ends = (condition is None and insn.ends_flow) or insn.call
            following = addr + insn.size
            if ends or following not in seen or following in leader_set:
                if not (condition is None and insn.ends_flow and not insn.call):
                    lines.append(f"  c.pc = 0x{following:X}u;")
                break
            addr = following
        blocks.append((start, bool(modes[start]), lines))

    files = []
    for index in range(0, len(blocks), args.per_file):
        part = blocks[index:index + args.per_file]
        name = f"code_{index // args.per_file:03}.cpp"
        files.append(name)
        with open(os.path.join(OUT, name), "w", newline="\n") as f:
            f.write('#include "cpu.h"\n\n')
            for start, _thumb, lines in part:
                f.write(f"void B_{start:08X}(Cpu& c) {{\n")
                f.write("\n".join(lines))
                f.write("\n}\n\n")

    with open(os.path.join(OUT, "table.cpp"), "w", newline="\n") as f:
        f.write('#include "cpu.h"\n#include <algorithm>\n#include <iterator>\n\n')
        for start, _thumb, _lines in blocks:
            f.write(f"void B_{start:08X}(Cpu&);\n")
        f.write("\nnamespace {\nstruct Entry { u32 key; BlockFn fn; };\n")
        f.write("const Entry kTable[] = {\n")
        for start, thumb, _lines in sorted(blocks, key=lambda b: b[0] | int(b[1])):
            f.write(f"  {{0x{start | int(thumb):X}u, B_{start:08X}}},\n")
        f.write("};\n}  // namespace\n\n")
        f.write("BlockFn find_block(u32 key) {\n"
                "  const Entry* end = std::end(kTable);\n"
                "  const Entry* hit = std::lower_bound(std::begin(kTable), end, key,\n"
                "      [](const Entry& e, u32 k) { return e.key < k; });\n"
                "  return (hit != end && hit->key == key) ? hit->fn : nullptr;\n}\n")
    files.append("table.cpp")
    print(f"blocks: {len(blocks)}, files: {len(files)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
