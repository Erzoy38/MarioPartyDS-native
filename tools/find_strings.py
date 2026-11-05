#!/usr/bin/env python3
"""List printable ASCII strings in a file that match a regular expression.

Reads the file only; nothing in it is run. Usage:
python tools/find_strings.py <file> <regex> [--limit N]
"""
import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file")
    ap.add_argument("regex")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()
    with open(args.file, "rb") as f:
        data = f.read()
    wanted = re.compile(args.regex, re.IGNORECASE)
    found = []
    for match in re.finditer(rb"[\x20-\x7e]{4,}", data):
        text = match.group().decode("ascii")
        if wanted.search(text):
            found.append((match.start(), text))
    print(f"{len(found)} match(es)")
    for offset, text in found[:args.limit]:
        print(f"{offset:#010x}  {text[:110]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
