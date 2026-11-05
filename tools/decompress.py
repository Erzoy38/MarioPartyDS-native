#!/usr/bin/env python3
"""Ph2: unpack ARM9, overlays and data files, verifying every result.

Run tools/nds_header.py first; this reads nds_files.csv and nds_overlays.csv.

Usage: python tools/decompress.py <rom.nds> [--out-dir unpacked]
Writes the unpacked tree plus unpack_report.csv to the current directory.
"""
import argparse
import collections
import csv
import os
import struct
import sys


class DecodeError(Exception):
    pass


def blz_decompress(data):
    """Backward LZ used for ARM9 and overlays. Returns the full unpacked image."""
    end = len(data)
    word0, inc = struct.unpack_from("<II", data, end - 8)
    comp_len, footer_len = word0 & 0xFFFFFF, word0 >> 24
    if not 8 <= footer_len <= 0x20 or comp_len > end:
        raise DecodeError(f"bad footer comp_len={comp_len:#x} footer={footer_len:#x}")
    bottom = end - comp_len
    src = end - footer_len
    dst = end + inc
    out = bytearray(data) + bytearray(inc)
    while src > bottom:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= bottom:
                break
            if flags & 0x80:
                if src - 2 < bottom:
                    raise DecodeError("truncated match")
                value = (out[src - 1] << 8) | out[src - 2]
                src -= 2
                length = (value >> 12) + 3
                disp = (value & 0xFFF) + 3
                if dst - length < src:
                    raise DecodeError("output overran input")
                for _ in range(length):
                    dst -= 1
                    if dst + disp >= len(out):
                        raise DecodeError("match past end")
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    if dst != src:
        raise DecodeError(f"pointers did not meet: dst={dst:#x} src={src:#x}")
    return bytes(out)


def lz10_decompress(data):
    """Forward LZ10. Returns (output, bytes_consumed)."""
    if len(data) < 4 or data[0] != 0x10:
        raise DecodeError("no LZ10 tag")
    size = int.from_bytes(data[1:4], "little")
    out = bytearray()
    pos = 4
    try:
        while len(out) < size:
            flags = data[pos]
            pos += 1
            for bit in range(7, -1, -1):
                if len(out) >= size:
                    break
                if flags >> bit & 1:
                    b1, b2 = data[pos], data[pos + 1]
                    pos += 2
                    length = (b1 >> 4) + 3
                    disp = (((b1 & 0xF) << 8) | b2) + 1
                    if disp > len(out):
                        raise DecodeError("match before start of output")
                    for _ in range(length):
                        out.append(out[-disp])
                else:
                    out.append(data[pos])
                    pos += 1
    except IndexError:
        raise DecodeError("input ended early") from None
    if len(out) != size:
        raise DecodeError(f"size overshoot {len(out):#x} != {size:#x}")
    return bytes(out), pos


def huff_decompress(data):
    """BIOS-style Huffman. Tag 0x28 = 8-bit symbols, 0x24 = 4-bit.

    Returns (output, bytes_consumed).
    """
    if len(data) < 6 or data[0] not in (0x24, 0x28):
        raise DecodeError("no Huffman tag")
    symbol_bits = data[0] & 0x0F
    size = int.from_bytes(data[1:4], "little")
    table = data[4:4 + (data[4] + 1) * 2]
    stream = 4 + len(table)
    out = bytearray()
    node = 1
    pending = 0
    pending_bits = 0
    pos = stream
    while len(out) < size:
        if pos + 4 > len(data):
            raise DecodeError("input ended early")
        word = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        for bit in range(31, -1, -1):
            if node >= len(table):
                raise DecodeError("tree index out of range")
            value = table[node]
            child = (node & ~1) + (value & 0x3F) * 2 + 2
            go_right = word >> bit & 1
            is_leaf = value & (0x40 if go_right else 0x80)
            node = child + go_right
            if not is_leaf:
                continue
            if node >= len(table):
                raise DecodeError("leaf index out of range")
            symbol = table[node]
            node = 1
            if symbol_bits == 8:
                out.append(symbol)
            else:
                pending |= symbol << pending_bits
                pending_bits += 4
                if pending_bits == 8:
                    out.append(pending)
                    pending = pending_bits = 0
            if len(out) >= size:
                break
    return bytes(out), pos


def parse_container(data):
    """Test the hypothesis: u32 N, then N (offset, size) pairs, offsets based at +4."""
    if len(data) < 12:
        return None, "too small"
    count = struct.unpack_from("<I", data, 0)[0]
    table_end = 4 + count * 8
    if count == 0 or table_end > len(data):
        return None, f"count {count:#x} does not fit"
    entries = []
    expected = table_end
    for i in range(count):
        off, size = struct.unpack_from("<II", data, 4 + i * 8)
        real = off + 4
        # Members are stored in order, aligned to 4 bytes.
        if real != (expected + 3) & ~3 and real != expected:
            return None, f"entry {i}: offset {real:#x}, expected {expected:#x}"
        if real + size > len(data):
            return None, f"entry {i}: runs past end of file"
        entries.append((real, size))
        expected = real + size
    slack = len(data) - expected
    if slack > 3:
        return None, f"{slack:#x} bytes left after last entry"
    return entries, "ok"


def write(root, rel, data):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom")
    ap.add_argument("--out-dir", default="unpacked")
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()
    report = []
    root = args.out_dir

    # ARM9, checked against the module params in the header region.
    off, _, _, size = struct.unpack_from("<4I", rom, 0x20)
    arm9_raw = rom[off:off + size]
    arm9 = blz_decompress(arm9_raw)
    write(root, "arm9.bin", arm9)
    idx = arm9.find(struct.pack("<II", 0xDEC00621, 0x2106C0DE))
    ram = struct.unpack_from("<I", rom, 0x28)[0]
    list_start, list_end, auto_start = struct.unpack_from("<3I", arm9, idx - 0x1C)
    print(f"arm9       {len(arm9_raw):#x} -> {len(arm9):#x}  "
          f"end_matches_autoload_list_end={ram + len(arm9) == list_end}")
    report.append(["arm9", len(arm9_raw), len(arm9), "blz", "ok"])

    # ARM7 is stored as is; the header gives its location.
    off7, _, _, size7 = struct.unpack_from("<4I", rom, 0x30)
    write(root, "arm7.bin", rom[off7:off7 + size7])
    report.append(["arm7", size7, size7, "stored", "ok"])

    # The autoload sizes must add up to the gap between autoload_start and the list.
    total = 0
    print("autoload entries (dest, size, bss):")
    for pos in range(list_start - ram, list_end - ram, 12):
        dest, length, bss = struct.unpack_from("<3I", arm9, pos)
        total += length
        print(f"  dest={dest:#010x} size={length:#x} bss={bss:#x}")
    print(f"  sum={total:#x} expected={list_start - auto_start:#x} "
          f"match={total == list_start - auto_start}")

    # Overlays, checked against ram_size and static-init pointers.
    with open("nds_overlays.csv", newline="") as f:
        overlays = list(csv.DictReader(f))
    good = bad = sinit_ok = sinit_bad = 0
    for ov in overlays:
        ov_id = int(ov["id"])
        start, rom_size = int(ov["rom_off"], 16), int(ov["rom_size"], 16)
        ram_addr, ram_size = int(ov["ram"], 16), int(ov["ram_size"], 16)
        raw = rom[start:start + rom_size]
        method = "stored"
        status = "ok"
        try:
            if ov["compressed_flag"] == "1":
                method = "blz"
                comp = int(ov["compressed_size"], 16) or rom_size
                out = blz_decompress(raw[:comp])
            else:
                out = raw
            if len(out) != ram_size:
                raise DecodeError(f"size {len(out):#x} != ram_size {ram_size:#x}")
            s0, s1 = int(ov["sinit_start"], 16), int(ov["sinit_end"], 16)
            for addr in range(s0, s1, 4):
                ptr = struct.unpack_from("<I", out, addr - ram_addr)[0]
                if ram_addr <= ptr < ram_addr + ram_size:
                    sinit_ok += 1
                else:
                    sinit_bad += 1
            write(root, f"overlay/ov{ov_id:03}.bin", out)
            good += 1
        except (DecodeError, struct.error) as exc:
            status = str(exc)
            bad += 1
            out = b""
            print(f"  ov{ov_id:03} FAILED: {status}")
        report.append([f"ov{ov_id:03}", rom_size, len(out), method, status])
    print(f"overlays   ok={good} failed={bad}  "
          f"sinit_pointers_in_range={sinit_ok} out_of_range={sinit_bad}")

    # Data files.
    with open("nds_files.csv", newline="") as f:
        files = [r for r in csv.DictReader(f) if r["path"].startswith("data/")]
    lz_ok = lz_bad = input_total = output_total = 0
    trailing = collections.Counter()
    cont_ok = 0
    cont_fail = []
    member_tags = collections.Counter()
    member_lz_ok = member_lz_bad = 0
    plain = 0
    huff_ok = huff_bad = 0
    huff_trailing = collections.Counter()
    for row in files:
        path = row["path"]
        start, size = int(row["rom_off"], 16), int(row["size"])
        data = rom[start:start + size]
        labeled = "_lz" in path.lower()
        if data[:1] == b"\x10" and labeled:
            try:
                out, used = lz10_decompress(data)
                trailing[size - used] += 1
                write(root, path, out)
                report.append([path, size, len(out), "lz10", "ok"])
                lz_ok += 1
                input_total += size
                output_total += len(out)
            except DecodeError as exc:
                lz_bad += 1
                report.append([path, size, 0, "lz10", str(exc)])
                print(f"  {path} FAILED: {exc}")
        elif data[:1] == b"\x28" and labeled:
            try:
                out, used = huff_decompress(data)
                huff_trailing[size - used] += 1
                write(root, path, out)
                report.append([path, size, len(out), "huff8", "ok"])
                huff_ok += 1
            except DecodeError as exc:
                huff_bad += 1
                report.append([path, size, 0, "huff8", str(exc)])
                print(f"  {path} FAILED: {exc}")
        elif labeled:
            entries, why = parse_container(data)
            if entries is None:
                cont_fail.append((path, why, data[:16].hex(" ")))
                write(root, path, data)
                report.append([path, size, size, "unknown", why])
                continue
            cont_ok += 1
            for i, (real, length) in enumerate(entries):
                member = data[real:real + length]
                member_tags[member[0] if member else -1] += 1
                out = member
                suffix = ".bin"
                if member[:1] == b"\x10":
                    try:
                        out, _ = lz10_decompress(member)
                        member_lz_ok += 1
                    except DecodeError:
                        member_lz_bad += 1
                        suffix = ".lz_failed"
                write(root, f"{path}.d/{i:03}{suffix}", out)
            report.append([path, size, size, "container", f"{len(entries)} members"])
        else:
            plain += 1
            write(root, path, data)
            report.append([path, size, size, "stored", "ok"])

    print(f"lz10 files ok={lz_ok} failed={lz_bad}  "
          f"{input_total} -> {output_total} bytes")
    print(f"           unused bytes after stream: {dict(sorted(trailing.items()))}")
    print(f"huff8      ok={huff_ok} failed={huff_bad}  "
          f"unused bytes: {dict(sorted(huff_trailing.items()))}")
    print(f"containers matched={cont_ok} not_matched={len(cont_fail)}")
    print(f"           member first bytes: "
          + ", ".join(f"{t:#04x}:{c}" for t, c in member_tags.most_common(8)))
    print(f"           members lz10 ok={member_lz_ok} failed={member_lz_bad}")
    for path, why, head in cont_fail[:15]:
        print(f"  not matched: {path}: {why}\n    {head}")
    print(f"stored     {plain} files without _LZ in the name")

    with open("unpack_report.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "input_size", "output_size", "method", "status"])
        w.writerows(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
