#!/usr/bin/env python3
"""Targeted ARM/Thumb disassembler. Loads labels.csv automatically.

Usage: python tools/disasm.py <addr> [lines] [--thumb] [--ov N] [--arm7] [--func]

  --thumb  decode as Thumb (default ARM; the mode is not stored in the binary)
  --ov N   map overlay N, required for addresses in the overlay area
  --arm7   use the ARM7 program instead of ARM9
  --func   stop at the end of the control flow (useful for one function)
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from instruction_set import decode_arm, decode_thumb  # noqa: E402
from nds_image import Space, parse_addr  # noqa: E402


def decode_at(space, addr, thumb):
    if thumb:
        first = space.u16(addr)
        if first is None:
            return None
        return decode_thumb(first, space.u16(addr + 2), addr)
    word = space.u32(addr)
    return None if word is None else decode_arm(word, addr)


def describe_value(space, value):
    """Short note about what a loaded constant might be."""
    notes = []
    known = space.label(value)
    if known:
        notes.append(known[0])
    seg = space.segment_at(value & ~1)
    if seg is not None:
        notes.append(seg.name)
        data = space.read(value, 32)
        if data:
            text = data.split(b"\0")[0]
            if len(text) >= 4 and all(0x20 <= b < 0x7F for b in text):
                notes.append(repr(text.decode("ascii")))
    return " ".join(notes)


def annotate(space, insn):
    notes = []
    if insn.target is not None:
        known = space.label(insn.target)
        if known:
            notes.append(known[0])
        if space.segment_at(insn.target) is None:
            notes.append("not mapped")
        notes.append("thumb" if insn.target_thumb else "arm")
    if insn.literal is not None:
        value = space.u32(insn.literal)
        if value is None:
            notes.append(f"={insn.literal:#010x}: not mapped")
        else:
            extra = describe_value(space, value)
            notes.append(f"={value:#010x}" + (f" {extra}" if extra else ""))
    if insn.ref is not None:
        extra = describe_value(space, insn.ref)
        notes.append(f"-> {insn.ref:#010x}" + (f" {extra}" if extra else ""))
    return "  ; " + " ".join(notes) if notes else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr")
    ap.add_argument("lines", nargs="?", type=int, default=32)
    ap.add_argument("--thumb", action="store_true")
    ap.add_argument("--ov", type=int)
    ap.add_argument("--arm7", action="store_true")
    ap.add_argument("--func", action="store_true")
    args = ap.parse_args()

    space = Space(ov=args.ov, arm7=args.arm7)
    addr = parse_addr(args.addr)
    if space.segment_at(addr) is None:
        hint = " (overlay area: pass --ov N)" if 0x021638A0 <= addr < 0x021A4FE0 else ""
        print(f"{addr:#010x} is not mapped{hint}", file=sys.stderr)
        return 1

    furthest = addr
    for _ in range(args.lines):
        insn = decode_at(space, addr, args.thumb)
        if insn is None:
            print(f"{addr:#010x}: end of segment")
            break
        known = space.label(addr)
        if known:
            print(f"\n{known[0]}:" + (f"  ; {known[1]}" if known[1] else ""))
        width = 8 if insn.size == 4 else 4
        print(f"{addr:#010x}  {insn.raw:0{width}x}{' ' * (9 - width)}"
              f"{insn.text:36}{annotate(space, insn)}")
        if insn.target is not None and insn.jump and not insn.call:
            furthest = max(furthest, insn.target)
        if args.func and insn.ends_flow and addr >= furthest:
            break
        addr += insn.size
    return 0


if __name__ == "__main__":
    sys.exit(main())
