#!/usr/bin/env python3
"""Shows how loud a WAV from the native run is, second by second, and whether it clips.

A plain check that sound comes out at all; whether it sounds right takes a listener.
Usage: python tools/wav_summary.py <file.wav>
"""
import array
import math
import sys


def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    samples = array.array("h")
    samples.frombytes(data[44:44 + (len(data) - 44) // 2 * 2])
    rate = int.from_bytes(data[24:28], "little")
    per_second = rate * 2
    print(f"{len(samples) / per_second:.1f} s at {rate} Hz")
    for start in range(0, len(samples), per_second):
        part = samples[start:start + per_second]
        if not part:
            break
        rms = math.sqrt(sum(s * s for s in part) / len(part))
        peak = max(abs(s) for s in part)
        clipped = sum(1 for s in part if abs(s) >= 32767)
        bar = "#" * int(rms / 400)
        print(f"{start // per_second:4}s rms {rms:7.0f} peak {peak:6} clipped {clipped:5} {bar}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
