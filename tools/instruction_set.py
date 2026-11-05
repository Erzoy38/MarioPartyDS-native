#!/usr/bin/env python3
"""ARMv5TE and Thumb instruction decoder for the Nintendo DS.

decode_arm(word, addr) and decode_thumb(halfword, next_halfword, addr) return
an Insn with the text form plus the control-flow facts the other tools need.
"""


class Insn:
    __slots__ = ("addr", "size", "raw", "mnem", "ops", "target", "target_thumb",
                 "literal", "ref", "call", "jump", "ret", "indirect", "cond",
                 "undefined", "rd", "reg_target", "clobbers_all")

    def __init__(self, addr, size, raw, thumb):
        self.addr = addr
        self.size = size
        self.raw = raw
        self.mnem = ""
        self.ops = ""
        self.target = None        # direct branch or call destination
        self.target_thumb = thumb  # instruction set at the destination
        self.literal = None       # address of a PC-relative loaded word
        self.ref = None           # address computed from PC (adr)
        self.call = False
        self.jump = False         # branch that is not a call
        self.ret = False
        self.indirect = False     # branch through a register or memory
        self.cond = False         # executes conditionally, so flow may fall through
        self.undefined = False
        self.rd = None            # destination register, when there is exactly one
        self.reg_target = None    # register holding the destination of bx/blx/mov pc
        self.clobbers_all = False

    @property
    def ends_flow(self):
        if self.undefined:
            return True
        if self.cond or self.call:
            return False
        return self.jump or self.ret or self.indirect

    @property
    def text(self):
        return f"{self.mnem} {self.ops}".rstrip()


COND = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt",
        "gt", "le", "", "nv"]
REG = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
       "r12", "sp", "lr", "pc"]
SHIFT = ["lsl", "lsr", "asr", "ror"]
DP = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp",
      "cmn", "orr", "mov", "bic", "mvn"]
PC = 15
SP = 13
LR = 14


def sext(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def ror32(value, amount):
    amount &= 31
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF if amount else value


def hexs(value):
    return f"#{value:#x}" if value > 9 else f"#{value}"


def reglist(mask):
    parts = []
    i = 0
    while i < 16:
        if mask >> i & 1:
            j = i
            while j + 1 < 16 and mask >> (j + 1) & 1:
                j += 1
            parts.append(REG[i] if i == j else f"{REG[i]}-{REG[j]}")
            i = j + 1
        else:
            i += 1
    return "{" + ",".join(parts) + "}"


def _undef(insn):
    insn.mnem = ".undefined"
    insn.ops = f"{insn.raw:#x}"
    insn.undefined = True
    return insn


def _shifted(w):
    rm = REG[w & 15]
    kind = (w >> 5) & 3
    if w >> 4 & 1:
        return f"{rm},{SHIFT[kind]} {REG[(w >> 8) & 15]}"
    amount = (w >> 7) & 31
    if amount == 0:
        if kind == 0:
            return rm
        if kind == 3:
            return f"{rm},rrx"
        amount = 32
    return f"{rm},{SHIFT[kind]} #{amount}"


def _dest_pc(insn, is_ret, reg=None):
    """Bookkeeping for an instruction that writes PC."""
    if is_ret:
        insn.ret = True
    else:
        insn.indirect = True
        insn.reg_target = reg


def decode_arm(w, addr):
    insn = Insn(addr, 4, w, False)
    cond = w >> 28
    c = COND[cond]
    pc = (addr + 8) & 0xFFFFFFFF
    insn.cond = cond not in (14, 15)

    if cond == 15:
        return _arm_unconditional(insn, w, pc)

    top = (w >> 26) & 3
    if top == 0:
        return _arm_group0(insn, w, c, pc)
    if top == 1:
        return _arm_load_store(insn, w, c, pc)
    if (w >> 25) & 7 == 4:
        return _arm_block(insn, w, c)
    if (w >> 25) & 7 == 5:
        link = w >> 24 & 1
        insn.mnem = ("bl" if link else "b") + c
        insn.target = (pc + (sext(w & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
        insn.ops = f"{insn.target:#010x}"
        insn.call = bool(link)
        insn.jump = not link
        return insn
    return _arm_coprocessor(insn, w, c)


def _arm_unconditional(insn, w, pc):
    if (w & 0xFE000000) == 0xFA000000:
        offset = (sext(w & 0xFFFFFF, 24) << 2) | ((w >> 23) & 2)
        insn.mnem = "blx"
        insn.target = (pc + offset) & 0xFFFFFFFF
        insn.target_thumb = True
        insn.ops = f"{insn.target:#010x}"
        insn.call = True
        return insn
    if (w & 0xFD70F000) == 0xF550F000:
        sign = "" if w >> 23 & 1 else "-"
        rn = REG[(w >> 16) & 15]
        if w >> 25 & 1:
            insn.mnem, insn.ops = "pld", f"[{rn},{sign}{_shifted(w)}]"
        else:
            insn.mnem, insn.ops = "pld", f"[{rn},#{sign}{w & 0xFFF:#x}]"
        return insn
    if (w & 0x0F000000) == 0x0E000000:
        insn.cond = False
        return _arm_coprocessor(insn, w, "2")
    if (w & 0x0E000000) == 0x0C000000:
        return _arm_coprocessor(insn, w, "2")
    return _undef(insn)


def _arm_group0(insn, w, c, pc):
    rd = (w >> 12) & 15
    rn = (w >> 16) & 15
    rs = (w >> 8) & 15
    rm = w & 15

    if (w & 0x0FFFFFF0) == 0x012FFF10:
        insn.mnem = "bx" + c
        insn.ops = REG[rm]
        if rm == LR:
            insn.ret = True
        else:
            insn.indirect = True
            insn.reg_target = rm
        return insn
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        insn.mnem = "blx" + c
        insn.ops = REG[rm]
        insn.call = True
        insn.indirect = True
        insn.reg_target = rm
        return insn
    if (w & 0x0FFF0FF0) == 0x016F0F10:
        insn.mnem, insn.ops, insn.rd = "clz" + c, f"{REG[rd]},{REG[rm]}", rd
        return insn
    if (w & 0xFFF000F0) == 0xE1200070:
        insn.mnem = "bkpt"
        insn.ops = hexs(((w >> 4) & 0xFFF0) | (w & 15))
        return insn
    if (w & 0x0F900FF0) == 0x01000050:
        name = ["qadd", "qsub", "qdadd", "qdsub"][(w >> 21) & 3]
        insn.mnem, insn.ops, insn.rd = name + c, f"{REG[rd]},{REG[rm]},{REG[rn]}", rd
        return insn
    if (w & 0x0F900090) == 0x01000080:
        return _arm_dsp_multiply(insn, w, c)
    if (w & 0x0FC000F0) == 0x00000090:
        s = "s" if w >> 20 & 1 else ""
        insn.rd = rn
        if w >> 21 & 1:
            insn.mnem = "mla" + c + s
            insn.ops = f"{REG[rn]},{REG[rm]},{REG[rs]},{REG[rd]}"
        else:
            insn.mnem = "mul" + c + s
            insn.ops = f"{REG[rn]},{REG[rm]},{REG[rs]}"
        return insn
    if (w & 0x0F8000F0) == 0x00800090:
        name = ("s" if w >> 22 & 1 else "u") + ("mlal" if w >> 21 & 1 else "mull")
        insn.mnem = name + c + ("s" if w >> 20 & 1 else "")
        insn.ops = f"{REG[rd]},{REG[rn]},{REG[rm]},{REG[rs]}"
        insn.clobbers_all = True
        return insn
    if (w & 0x0FB00FF0) == 0x01000090:
        insn.mnem = "swp" + c + ("b" if w >> 22 & 1 else "")
        insn.ops = f"{REG[rd]},{REG[rm]},[{REG[rn]}]"
        insn.rd = rd
        return insn
    if (w & 0x0E000090) == 0x00000090:
        return _arm_extra_load_store(insn, w, c, pc)
    if (w & 0x0FBF0FFF) == 0x010F0000:
        insn.mnem = "mrs" + c
        insn.ops = f"{REG[rd]},{'spsr' if w >> 22 & 1 else 'cpsr'}"
        insn.rd = rd
        return insn
    if (w & 0x0FB0FFF0) == 0x0120F000 or (w & 0x0FB0F000) == 0x0320F000:
        field = "".join(ch for bit, ch in zip(range(16, 20), "cxsf") if w >> bit & 1)
        psr = "spsr" if w >> 22 & 1 else "cpsr"
        insn.mnem = "msr" + c
        if w >> 25 & 1:
            value = ror32(w & 0xFF, ((w >> 8) & 15) * 2)
            insn.ops = f"{psr}_{field},{hexs(value)}"
        else:
            insn.ops = f"{psr}_{field},{REG[rm]}"
        return insn
    return _arm_data_processing(insn, w, c, pc)


def _arm_dsp_multiply(insn, w, c):
    op = (w >> 21) & 3
    x = "t" if w >> 5 & 1 else "b"
    y = "t" if w >> 6 & 1 else "b"
    rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
    insn.rd = rd
    if op == 0:
        insn.mnem = f"smla{x}{y}{c}"
        insn.ops = f"{REG[rd]},{REG[rm]},{REG[rs]},{REG[rn]}"
    elif op == 1:
        if w >> 5 & 1:
            insn.mnem = f"smulw{y}{c}"
            insn.ops = f"{REG[rd]},{REG[rm]},{REG[rs]}"
        else:
            insn.mnem = f"smlaw{y}{c}"
            insn.ops = f"{REG[rd]},{REG[rm]},{REG[rs]},{REG[rn]}"
    elif op == 2:
        insn.mnem = f"smlal{x}{y}{c}"
        insn.ops = f"{REG[rn]},{REG[rd]},{REG[rm]},{REG[rs]}"
        insn.clobbers_all = True
    else:
        insn.mnem = f"smul{x}{y}{c}"
        insn.ops = f"{REG[rd]},{REG[rm]},{REG[rs]}"
    return insn


def _arm_extra_load_store(insn, w, c, pc):
    p, u, imm, wb, load = (w >> 24 & 1, w >> 23 & 1, w >> 22 & 1, w >> 21 & 1, w >> 20 & 1)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    op = (w >> 5) & 3
    if op == 0:
        # Multiply-space encodings that matched no valid multiply or swap.
        return _undef(insn)
    if load:
        name = {1: "ldrh", 2: "ldrsb", 3: "ldrsh"}[op]
    else:
        name = {1: "strh", 2: "ldrd", 3: "strd"}[op]
    is_dword = not load and op in (2, 3)
    if is_dword and rd & 1:
        return _undef(insn)
    insn.mnem = name + c
    sign = "" if u else "-"
    if imm:
        offset = ((w >> 4) & 0xF0) | (w & 15)
        if p and rn == PC and not wb:
            insn.literal = (pc + offset if u else pc - offset) & 0xFFFFFFFF
        addend = f",#{sign}{offset:#x}" if offset else ""
    else:
        addend = f",{sign}{REG[w & 15]}"
    if p:
        mem = f"[{REG[rn]}{addend}]" + ("!" if wb else "")
    else:
        mem = f"[{REG[rn]}]{addend}"
    insn.ops = f"{REG[rd]},{mem}"
    if load or op == 2:
        insn.rd = rd
    if op == 2 and not load:
        insn.clobbers_all = True
    return insn


def _arm_data_processing(insn, w, c, pc):
    opcode = (w >> 21) & 15
    s = w >> 20 & 1
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    is_imm = w >> 25 & 1
    if 8 <= opcode <= 11 and not s:
        return _undef(insn)
    if is_imm:
        value = ror32(w & 0xFF, ((w >> 8) & 15) * 2)
        operand = hexs(value)
    else:
        value = None
        operand = _shifted(w)
    name = DP[opcode]
    if 8 <= opcode <= 11:
        insn.mnem = name + c
        insn.ops = f"{REG[rn]},{operand}"
        return insn
    insn.mnem = name + c + ("s" if s else "")
    insn.rd = rd
    if opcode in (13, 15):
        insn.ops = f"{REG[rd]},{operand}"
    else:
        insn.ops = f"{REG[rd]},{REG[rn]},{operand}"
    if value is not None and rn == PC and opcode in (2, 4):
        insn.ref = (pc + value if opcode == 4 else pc - value) & 0xFFFFFFFF
    if rd == PC:
        rm = w & 15
        plain_mov = opcode == 13 and not is_imm and (w & 0xFF0) == 0
        _dest_pc(insn, plain_mov and rm == LR, rm if plain_mov else None)
    return insn


def _arm_load_store(insn, w, c, pc):
    is_reg, p, u, byte, wb, load = (w >> 25 & 1, w >> 24 & 1, w >> 23 & 1, w >> 22 & 1,
                                    w >> 21 & 1, w >> 20 & 1)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    if is_reg and w >> 4 & 1:
        return _undef(insn)
    name = ("ldr" if load else "str") + c + ("b" if byte else "")
    if not p and wb:
        name += "t"
    insn.mnem = name
    sign = "" if u else "-"
    if is_reg:
        addend = f",{sign}{_shifted(w)}"
    else:
        offset = w & 0xFFF
        addend = f",#{sign}{offset:#x}" if offset else ""
        if p and rn == PC and not wb:
            insn.literal = (pc + offset if u else pc - offset) & 0xFFFFFFFF
    if p:
        mem = f"[{REG[rn]}{addend}]" + ("!" if wb else "")
    else:
        mem = f"[{REG[rn]}]{addend}"
    insn.ops = f"{REG[rd]},{mem}"
    if load:
        insn.rd = rd
        if rd == PC:
            _dest_pc(insn, rn == SP and not p and u)
    return insn


def _arm_block(insn, w, c):
    p, u, s, wb, load = (w >> 24 & 1, w >> 23 & 1, w >> 22 & 1, w >> 21 & 1, w >> 20 & 1)
    rn = (w >> 16) & 15
    mask = w & 0xFFFF
    suffix = ("i" if u else "d") + ("b" if p else "a")
    hat = "^" if s else ""
    if rn == SP and wb and load and suffix == "ia":
        insn.mnem, insn.ops = "pop" + c, reglist(mask) + hat
    elif rn == SP and wb and not load and suffix == "db":
        insn.mnem, insn.ops = "push" + c, reglist(mask) + hat
    else:
        insn.mnem = ("ldm" if load else "stm") + c + suffix
        insn.ops = f"{REG[rn]}{'!' if wb else ''},{reglist(mask)}{hat}"
    if load:
        insn.clobbers_all = True
        if mask >> PC & 1:
            _dest_pc(insn, rn == SP)
    return insn


def _arm_coprocessor(insn, w, c):
    if (w >> 25) & 7 == 6:
        p, u, n, wb, load = (w >> 24 & 1, w >> 23 & 1, w >> 22 & 1, w >> 21 & 1, w >> 20 & 1)
        rn, crd, cp, offset = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, (w & 0xFF) * 4
        sign = "" if u else "-"
        if p:
            mem = f"[{REG[rn]},#{sign}{offset:#x}]" + ("!" if wb else "")
        else:
            mem = f"[{REG[rn]}],#{sign}{offset:#x}"
        insn.mnem = ("ldc" if load else "stc") + c + ("l" if n else "")
        insn.ops = f"p{cp},c{crd},{mem}"
        return insn
    if w >> 24 & 1:
        insn.mnem = "swi" + c
        insn.ops = hexs(w & 0xFFFFFF)
        return insn
    crn, reg, cp, op2, crm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, (w >> 5) & 7, w & 15
    if w >> 4 & 1:
        load = w >> 20 & 1
        insn.mnem = ("mrc" if load else "mcr") + c
        insn.ops = f"p{cp},{(w >> 21) & 7},{REG[reg]},c{crn},c{crm},{op2}"
        if load and reg != PC:
            insn.rd = reg
    else:
        insn.mnem = "cdp" + c
        insn.ops = f"p{cp},{(w >> 20) & 15},c{reg},c{crn},c{crm},{op2}"
    return insn


THUMB_ALU = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg",
             "cmp", "cmn", "orr", "mul", "bic", "mvn"]
THUMB_REGOFF = ["str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"]


def decode_thumb(h, next_h, addr):
    insn = Insn(addr, 2, h, True)
    pc = (addr + 4) & 0xFFFFFFFF
    t = h >> 13
    rd3, rs3 = h & 7, (h >> 3) & 7

    if t == 0:
        op = (h >> 11) & 3
        if op < 3:
            amount = (h >> 6) & 31
            insn.rd = rd3
            if op == 0 and amount == 0:
                insn.mnem, insn.ops = "movs", f"{REG[rd3]},{REG[rs3]}"
            else:
                amount = amount or 32
                insn.mnem = ["lsls", "lsrs", "asrs"][op]
                insn.ops = f"{REG[rd3]},{REG[rs3]},#{amount}"
            return insn
        field = (h >> 6) & 7
        operand = f"#{field}" if h >> 10 & 1 else REG[field]
        insn.mnem = "subs" if h >> 9 & 1 else "adds"
        insn.ops = f"{REG[rd3]},{REG[rs3]},{operand}"
        insn.rd = rd3
        return insn

    if t == 1:
        op = (h >> 11) & 3
        rd = (h >> 8) & 7
        insn.mnem = ["movs", "cmp", "adds", "subs"][op]
        insn.ops = f"{REG[rd]},{hexs(h & 0xFF)}"
        if op != 1:
            insn.rd = rd
        return insn

    if h >> 10 == 0b010000:
        op = (h >> 6) & 15
        name = THUMB_ALU[op]
        if op in (8, 10, 11):
            insn.mnem = name
        else:
            insn.mnem = name + "s"
            insn.rd = rd3
        insn.ops = f"{REG[rd3]},{REG[rs3]}"
        return insn

    if h >> 10 == 0b010001:
        return _thumb_hireg(insn, h)

    if h >> 11 == 0b01001:
        rd = (h >> 8) & 7
        offset = (h & 0xFF) * 4
        insn.mnem = "ldr"
        insn.ops = f"{REG[rd]},[pc,#{offset:#x}]"
        insn.literal = (pc & ~3) + offset
        insn.rd = rd
        return insn

    if h >> 12 == 0b0101:
        op = (h >> 9) & 7
        insn.mnem = THUMB_REGOFF[op]
        insn.ops = f"{REG[rd3]},[{REG[rs3]},{REG[(h >> 6) & 7]}]"
        if op >= 3:
            insn.rd = rd3
        return insn

    if t == 0b011:
        byte, load = h >> 12 & 1, h >> 11 & 1
        offset = ((h >> 6) & 31) * (1 if byte else 4)
        insn.mnem = ("ldr" if load else "str") + ("b" if byte else "")
        insn.ops = f"{REG[rd3]},[{REG[rs3]},#{offset:#x}]"
        if load:
            insn.rd = rd3
        return insn

    top = h >> 12
    if top == 0b1000:
        load = h >> 11 & 1
        insn.mnem = "ldrh" if load else "strh"
        insn.ops = f"{REG[rd3]},[{REG[rs3]},#{((h >> 6) & 31) * 2:#x}]"
        if load:
            insn.rd = rd3
        return insn
    if top == 0b1001:
        load = h >> 11 & 1
        rd = (h >> 8) & 7
        insn.mnem = "ldr" if load else "str"
        insn.ops = f"{REG[rd]},[sp,#{(h & 0xFF) * 4:#x}]"
        if load:
            insn.rd = rd
        return insn
    if top == 0b1010:
        rd = (h >> 8) & 7
        offset = (h & 0xFF) * 4
        insn.mnem = "add"
        insn.rd = rd
        if h >> 11 & 1:
            insn.ops = f"{REG[rd]},sp,#{offset:#x}"
        else:
            insn.ops = f"{REG[rd]},pc,#{offset:#x}"
            insn.ref = (pc & ~3) + offset
        return insn
    if top == 0b1011:
        return _thumb_misc(insn, h)
    if top == 0b1100:
        load = h >> 11 & 1
        insn.mnem = "ldmia" if load else "stmia"
        insn.ops = f"{REG[(h >> 8) & 7]}!,{reglist(h & 0xFF)}"
        if load:
            insn.clobbers_all = True
        return insn
    if top == 0b1101:
        cond = (h >> 8) & 15
        if cond == 14:
            return _undef(insn)
        if cond == 15:
            insn.mnem, insn.ops = "swi", hexs(h & 0xFF)
            return insn
        insn.mnem = "b" + COND[cond]
        insn.target = (pc + (sext(h & 0xFF, 8) << 1)) & 0xFFFFFFFF
        insn.ops = f"{insn.target:#010x}"
        insn.jump = True
        insn.cond = True
        return insn

    five = h >> 11
    if five == 0b11100:
        insn.mnem = "b"
        insn.target = (pc + (sext(h & 0x7FF, 11) << 1)) & 0xFFFFFFFF
        insn.ops = f"{insn.target:#010x}"
        insn.jump = True
        return insn
    if five == 0b11110 and next_h is not None and next_h >> 11 in (0b11111, 0b11101):
        offset = (sext(h & 0x7FF, 11) << 12) + ((next_h & 0x7FF) << 1)
        target = (pc + offset) & 0xFFFFFFFF
        insn.size = 4
        insn.raw = (h << 16) | next_h
        insn.call = True
        if next_h >> 11 == 0b11111:
            insn.mnem = "bl"
        else:
            if next_h & 1:
                return _undef(insn)
            insn.mnem = "blx"
            target &= ~3
            insn.target_thumb = False
        insn.target = target
        insn.ops = f"{target:#010x}"
        return insn
    return _undef(insn)


def _thumb_hireg(insn, h):
    op = (h >> 8) & 3
    rd = (h & 7) | ((h >> 4) & 8)
    rm = (h >> 3) & 15
    if op == 3:
        if h & 7:
            return _undef(insn)
        if h >> 7 & 1:
            insn.mnem = "blx"
            insn.call = True
            insn.indirect = True
            insn.reg_target = rm
        else:
            insn.mnem = "bx"
            if rm == LR:
                insn.ret = True
            else:
                insn.indirect = True
                insn.reg_target = rm
        insn.ops = REG[rm]
        return insn
    insn.mnem = ["add", "cmp", "mov"][op]
    insn.ops = f"{REG[rd]},{REG[rm]}"
    if op != 1:
        insn.rd = rd
        if rd == PC:
            _dest_pc(insn, op == 2 and rm == LR, rm if op == 2 else None)
    return insn


def _thumb_misc(insn, h):
    if h >> 8 == 0xB0:
        insn.mnem = "sub" if h >> 7 & 1 else "add"
        insn.ops = f"sp,#{(h & 0x7F) * 4:#x}"
        return insn
    if (h >> 9) & 3 == 2:
        load = h >> 11 & 1
        extra = h >> 8 & 1
        mask = h & 0xFF
        if load:
            insn.mnem = "pop"
            insn.clobbers_all = True
            if extra:
                mask |= 1 << PC
                insn.ret = True
        else:
            insn.mnem = "push"
            if extra:
                mask |= 1 << LR
        insn.ops = reglist(mask)
        return insn
    if h >> 8 == 0xBE:
        insn.mnem, insn.ops = "bkpt", hexs(h & 0xFF)
        return insn
    return _undef(insn)
