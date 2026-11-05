#!/usr/bin/env python3
"""Recursive-descent code trace from an entry point.

Follows direct branches, calls and the mode switches of bx/blx, and resolves
register targets when a constant was loaded earlier in the same block. Reports
coverage and every undefined instruction it reaches, which is the main signal
of a decoder bug or of data mistaken for code.

Usage: python tools/trace_code.py [--arm7] [--ov N] [--entry ADDR[:t]]... [--pointers]
       [--csv functions.csv]

  --pointers  also follow literal-pool constants that point into mapped code.
              This is a guess, so its results are reported separately.
"""
import argparse
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import decode_at  # noqa: E402
from nds_image import ARM7_BASE, ARM9_BASE, Space, parse_addr  # noqa: E402


class Tracer:
    def __init__(self, space, code_segments):
        self.space = space
        self.code = code_segments
        self.seen = {}            # addr -> Insn
        self.mode_at = {}         # addr -> thumb flag
        self.conflicts = set()
        self.functions = {}       # (addr, thumb) -> source
        self.callers = collections.Counter()
        self.work = []
        self.undefined = []
        self.outside = collections.Counter()
        self.unresolved = 0
        self.resolved = 0
        self.literals = set()
        self.stage = "direct"

    def in_code(self, addr):
        return any(s.contains(addr) for s in self.code)

    def add(self, addr, thumb, source=None):
        if not self.in_code(addr):
            self.outside[self.region(addr)] += 1
            return
        if source is not None and (addr, thumb) not in self.functions:
            self.functions[(addr, thumb)] = source
        self.work.append((addr, thumb, self.stage))

    @staticmethod
    def region(addr):
        if 0x021638A0 <= addr < 0x021A4FE0:
            return "overlay area"
        if 0x04000000 <= addr < 0x05000000:
            return "I/O"
        if 0xFFFF0000 <= addr:
            return "BIOS"
        if 0x027E0000 <= addr < 0x02800000:
            return "DTCM"
        if 0x01000000 <= addr < 0x02000000:
            return "ITCM"
        if 0x02000000 <= addr < 0x03000000:
            return "main RAM"
        if 0x03000000 <= addr < 0x04000000:
            return "shared/ARM7 WRAM"
        return "other"

    def run(self):
        while self.work:
            addr, thumb, stage = self.work.pop()
            self.walk(addr, thumb, stage)

    def walk(self, addr, thumb, stage):
        regs = {}
        while True:
            if addr in self.seen:
                if self.mode_at[addr] != thumb:
                    self.conflicts.add(addr)
                return
            if not self.in_code(addr):
                self.outside[self.region(addr)] += 1
                return
            insn = decode_at(self.space, addr, thumb)
            if insn is None:
                return
            self.seen[addr] = insn
            self.mode_at[addr] = thumb
            if insn.undefined:
                self.undefined.append((addr, thumb, insn.raw, stage))
                return
            self.step_regs(insn, regs)
            self.follow(insn, thumb, regs)
            if insn.ends_flow:
                return
            addr += insn.size

    def step_regs(self, insn, regs):
        if insn.literal is not None:
            value = self.space.u32(insn.literal)
            if value is not None:
                self.literals.add((insn.literal, value))
            if insn.rd is not None and insn.mnem.startswith("ldr") and value is not None \
                    and insn.mnem in ("ldr",) and not insn.cond:
                regs[insn.rd] = value
                return
        if insn.ref is not None and insn.rd is not None and not insn.cond:
            regs[insn.rd] = insn.ref
            return
        if insn.clobbers_all:
            regs.clear()
        elif insn.rd is not None:
            regs.pop(insn.rd, None)

    def follow(self, insn, thumb, regs):
        if insn.target is not None:
            source = "call" if insn.call else None
            if insn.call:
                self.callers[insn.target] += 1
            self.add(insn.target, insn.target_thumb, source)
        if insn.reg_target is not None:
            value = regs.get(insn.reg_target)
            if value is None:
                self.unresolved += 1
            else:
                self.resolved += 1
                target_thumb = bool(value & 1)
                self.add(value & ~1, target_thumb, "indirect")
        elif insn.indirect:
            self.unresolved += 1
        if insn.call:
            for reg in (0, 1, 2, 3, 12, 14):
                regs.pop(reg, None)

    def seed_pointers(self):
        added = 0
        for _loc, value in sorted(self.literals):
            base = value & ~1
            if base in self.seen or not self.in_code(base):
                continue
            thumb = bool(value & 1)
            if not thumb and base & 3:
                continue
            self.add(base, thumb, "pointer")
            added += 1
        return added

    def stats(self):
        arm = thumb = covered = 0
        for addr, insn in self.seen.items():
            covered += insn.size
            if self.mode_at[addr]:
                thumb += 1
            else:
                arm += 1
        return arm, thumb, covered


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm7", action="store_true")
    ap.add_argument("--ov", type=int)
    ap.add_argument("--entry", action="append", default=[])
    ap.add_argument("--pointers", action="store_true")
    ap.add_argument("--csv")
    args = ap.parse_args()

    space = Space(ov=args.ov, arm7=args.arm7)
    code = [s for s in space.segments]
    tracer = Tracer(space, code)
    size = sum(len(s.data) for s in code)

    tracer.add(ARM7_BASE if args.arm7 else ARM9_BASE + 0x800, False, "entry")
    for spec in args.entry:
        text, _, mode = spec.partition(":")
        tracer.add(parse_addr(text), mode == "t", "entry")
    tracer.run()

    def report(title):
        arm, thumb, covered = tracer.stats()
        undefined = len(tracer.undefined)
        print(f"{title}: instructions arm={arm} thumb={thumb} undefined={undefined} "
              f"functions={len(tracer.functions)} "
              f"bytes={covered:#x}/{size:#x} ({100 * covered / size:.1f}%) "
              f"conflicts={len(tracer.conflicts)}")

    report("direct")
    print(f"  indirect branches: resolved={tracer.resolved} unresolved={tracer.unresolved}")
    direct_undefined = len(tracer.undefined)

    if args.pointers:
        tracer.stage = "pointer"
        rounds = 0
        while True:
            if not tracer.seed_pointers():
                break
            tracer.run()
            rounds += 1
        report(f"with pointers ({rounds} rounds)")
        print(f"  undefined from direct stage: {direct_undefined}, "
              f"from pointer stage: {len(tracer.undefined) - direct_undefined}")

    print("  targets outside mapped code: " + ", ".join(
        f"{k}={v}" for k, v in tracer.outside.most_common()))
    for addr, thumb, raw, stage in tracer.undefined[:12]:
        print(f"  undefined {addr:#010x} {'thumb' if thumb else 'arm'} {raw:#x} ({stage})")

    per_seg = collections.Counter()
    for addr, insn in tracer.seen.items():
        seg = space.segment_at(addr)
        per_seg[seg.name if seg else "?"] += insn.size
    print("  bytes by segment: " + ", ".join(
        f"{k}={v:#x}/{len(next(s.data for s in code if s.name == k)):#x}"
        for k, v in per_seg.items()))

    mnems = collections.Counter(i.mnem for i in tracer.seen.values())
    print("  top mnemonics: " + ", ".join(f"{m}:{c}" for m, c in mnems.most_common(14)))

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["addr", "mode", "source", "callers"])
            for (addr, thumb), source in sorted(tracer.functions.items()):
                w.writerow([f"{addr:#010x}", "thumb" if thumb else "arm", source,
                            tracer.callers[addr]])
        print(f"  wrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
