#!/usr/bin/env python3
"""Search the unpacked code images for a byte pattern. ?? matches any byte.

Usage: python tools/search_bytes.py <hex> [--context N] [--disasm [N]] [--thumb]
       [--ov N] [--arm7]

Without --ov or --arm7, searches the ARM9 program and all overlays.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import decode_at  # noqa: E402
from nds_image import Space, all_code_images  # noqa: E402


def compile_pattern(text):
    text = text.replace(" ", "")
    if len(text) % 2:
        raise SystemExit("hex pattern must have an even number of digits")
    parts = []
    for i in range(0, len(text), 2):
        pair = text[i:i + 2]
        parts.append(b"." if pair == "??" else re.escape(bytes.fromhex(pair)))
    return re.compile(b"".join(parts), re.DOTALL)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pattern")
    ap.add_argument("--context", type=int, default=0)
    ap.add_argument("--disasm", nargs="?", type=int, const=4)
    ap.add_argument("--thumb", action="store_true")
    ap.add_argument("--ov", type=int)
    ap.add_argument("--arm7", action="store_true")
    ap.add_argument("--limit", type=int, default=50)
    args = ap.parse_args()

    regex = compile_pattern(args.pattern)
    if args.arm7 or args.ov is not None:
        space = Space(ov=args.ov, arm7=args.arm7)
        images = [(s.name, s.base, s.data) for s in space.segments]
    else:
        images = all_code_images()

    hits = 0
    for tag, base, data in images:
        for match in regex.finditer(data):
            hits += 1
            if hits > args.limit:
                continue
            addr = base + match.start()
            print(f"{tag:6} {addr:#010x}  {match.group().hex(' ')}")
            if args.context:
                lo = max(match.start() - args.context, 0)
                print(f"       context from {base + lo:#010x}: "
                      f"{data[lo:match.end() + args.context].hex(' ')}")
            if args.disasm:
                ov = int(tag[2:]) if tag.startswith("ov") else None
                space = Space(ov=ov, arm7=args.arm7)
                cur = addr
                for _ in range(args.disasm):
                    insn = decode_at(space, cur, args.thumb)
                    if insn is None:
                        break
                    print(f"       {cur:#010x}  {insn.text}")
                    cur += insn.size
    if hits > args.limit:
        print(f"... {hits - args.limit} more (raise --limit)")
    print(f"{hits} match(es)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
