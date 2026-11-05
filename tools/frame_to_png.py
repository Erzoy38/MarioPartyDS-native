#!/usr/bin/env python3
"""Turns a frame.rgba from MarioPartyDS-headless (256 x 384, top screen first) into a PNG.

Usage: python tools/frame_to_png.py <frame.rgba> <out.png>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import write_png  # noqa: E402


def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    write_png(sys.argv[2], 256, 384, data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
