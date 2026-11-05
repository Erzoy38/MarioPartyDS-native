#!/usr/bin/env python3
"""Ph1 identification for Nintendo DS ROMs.

Parses the cartridge header, verifies its CRCs, locates the ARM9 module
params, and walks the overlay tables and NitroROM file system (FNT + FAT).

Usage: python tools/nds_header.py <rom.nds> [--out-dir DIR]
Writes nds_files.csv and nds_overlays.csv to --out-dir (default: current dir).
"""
import argparse
import collections
import csv
import os
import struct
import sys


def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def ascii_field(buf):
    return buf.split(b"\0")[0].decode("ascii", "replace")


def parse_header(rom):
    h = rom[:0x200]
    return {
        "title": ascii_field(h[0x00:0x0C]),
        "gamecode": ascii_field(h[0x0C:0x10]),
        "maker": ascii_field(h[0x10:0x12]),
        "unitcode": h[0x12],
        "devicecap": h[0x14],
        "romversion": h[0x1E],
        "arm9_off": u32(h, 0x20),
        "arm9_entry": u32(h, 0x24),
        "arm9_ram": u32(h, 0x28),
        "arm9_size": u32(h, 0x2C),
        "arm7_off": u32(h, 0x30),
        "arm7_entry": u32(h, 0x34),
        "arm7_ram": u32(h, 0x38),
        "arm7_size": u32(h, 0x3C),
        "fnt_off": u32(h, 0x40),
        "fnt_size": u32(h, 0x44),
        "fat_off": u32(h, 0x48),
        "fat_size": u32(h, 0x4C),
        "ovt9_off": u32(h, 0x50),
        "ovt9_size": u32(h, 0x54),
        "ovt7_off": u32(h, 0x58),
        "ovt7_size": u32(h, 0x5C),
        "icon_off": u32(h, 0x68),
        "arm9_autoload": u32(h, 0x70),
        "arm7_autoload": u32(h, 0x74),
        "rom_used": u32(h, 0x80),
        "header_size": u32(h, 0x84),
        "logo_crc": u16(h, 0x15C),
        "header_crc": u16(h, 0x15E),
    }


def find_module_params(arm9):
    """Module params end with the two nitrocode magic words."""
    marker = struct.pack("<II", 0xDEC00621, 0x2106C0DE)
    idx = arm9.find(marker)
    if idx < 0x1C:
        return None
    names = ["autoload_list", "autoload_list_end", "autoload_start",
             "static_bss_start", "static_bss_end", "compressed_static_end",
             "sdk_version"]
    start = idx - 0x1C
    params = {n: u32(arm9, start + i * 4) for i, n in enumerate(names)}
    params["params_file_off"] = start
    return params


def walk_fnt(fnt):
    """Return {file_id: path} from the file name table."""
    paths = {}

    def walk(dir_id, prefix):
        entry = (dir_id & 0xFFF) * 8
        pos = u32(fnt, entry)
        file_id = u16(fnt, entry + 4)
        while True:
            ctl = fnt[pos]
            pos += 1
            if ctl == 0:
                return
            name = fnt[pos:pos + (ctl & 0x7F)].decode("latin-1")
            pos += ctl & 0x7F
            if ctl & 0x80:
                child = u16(fnt, pos)
                pos += 2
                walk(child, prefix + name + "/")
            else:
                paths[file_id] = prefix + name
                file_id += 1

    walk(0xF000, "")
    return paths


def magic_of(data):
    """Classify a file by its first bytes."""
    if len(data) >= 4 and all(0x20 <= b < 0x7F for b in data[:4]):
        return data[:4].decode("ascii")
    if data[:1] == b"\x10" and len(data) >= 4:
        return "LZ10?"
    if data[:1] == b"\x11" and len(data) >= 4:
        return "LZ11?"
    if data[:1] == b"\x24" and len(data) >= 4:
        return "HUFF4?"
    if data[:1] == b"\x28" and len(data) >= 4:
        return "HUFF8?"
    if data[:1] == b"\x30" and len(data) >= 4:
        return "RLE?"
    return "(other)" if data else "(empty)"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom")
    ap.add_argument("--out-dir", default=".")
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()

    h = parse_header(rom)
    print(f"file size        {len(rom):#x} ({len(rom)} bytes)")
    print(f"rom used         {h['rom_used']:#x}  header size {h['header_size']:#x}")
    print(f"title            {h['title']!r}")
    print(f"gamecode/maker   {h['gamecode']} / {h['maker']}")
    print(f"unit/version     unit={h['unitcode']} rev={h['romversion']} devicecap={h['devicecap']}")
    print(f"header crc       stored={h['header_crc']:#06x} computed={crc16(rom[:0x15E]):#06x}")
    print(f"logo crc         stored={h['logo_crc']:#06x} computed={crc16(rom[0xC0:0x15C]):#06x}")
    for cpu in ("arm9", "arm7"):
        print(f"{cpu}             off={h[cpu + '_off']:#x} size={h[cpu + '_size']:#x} "
              f"ram={h[cpu + '_ram']:#x} entry={h[cpu + '_entry']:#x} "
              f"autoload={h[cpu + '_autoload']:#x}")
    print(f"fnt              off={h['fnt_off']:#x} size={h['fnt_size']:#x}")
    print(f"fat              off={h['fat_off']:#x} size={h['fat_size']:#x}")
    print(f"ovt9             off={h['ovt9_off']:#x} size={h['ovt9_size']:#x}")
    print(f"ovt7             off={h['ovt7_off']:#x} size={h['ovt7_size']:#x}")
    print(f"icon/title       off={h['icon_off']:#x}")

    arm9 = rom[h["arm9_off"]:h["arm9_off"] + h["arm9_size"] + 0x100]
    print(f"arm9 secure area first 8 bytes: {arm9[:8].hex()}")
    params = find_module_params(arm9)
    if params is None:
        print("arm9 module params: NOT FOUND")
    else:
        print("arm9 module params:")
        for key, val in params.items():
            print(f"  {key:24}{val:#x}")
        sdk = params["sdk_version"]
        print(f"  sdk decoded             {sdk >> 24}.{(sdk >> 16) & 0xFF}.{sdk & 0xFFFF}")
        print(f"  arm9 compressed         {params['compressed_static_end'] != 0}")

    fat = rom[h["fat_off"]:h["fat_off"] + h["fat_size"]]
    fnt = rom[h["fnt_off"]:h["fnt_off"] + h["fnt_size"]]
    paths = walk_fnt(fnt)
    file_count = h["fat_size"] // 8

    overlay_files = {}
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "nds_overlays.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cpu", "id", "ram", "ram_size", "bss_size", "sinit_start",
                    "sinit_end", "file_id", "rom_off", "rom_size",
                    "compressed_size", "compressed_flag"])
        for cpu, off, size in (("arm9", h["ovt9_off"], h["ovt9_size"]),
                               ("arm7", h["ovt7_off"], h["ovt7_size"])):
            count = size // 32
            total = compressed = 0
            ram_lo, ram_hi = 0xFFFFFFFF, 0
            for i in range(count):
                vals = struct.unpack_from("<8I", rom, off + i * 32)
                ov_id, ram, ram_size, bss, s0, s1, fid, flags = vals
                start, end = struct.unpack_from("<II", fat, fid * 8)
                overlay_files[fid] = f"overlay{'9' if cpu == 'arm9' else '7'}_{ov_id}"
                is_comp = (flags >> 24) & 1
                compressed += is_comp
                total += end - start
                ram_lo, ram_hi = min(ram_lo, ram), max(ram_hi, ram + ram_size + bss)
                w.writerow([cpu, ov_id, hex(ram), hex(ram_size), hex(bss), hex(s0),
                            hex(s1), fid, hex(start), hex(end - start),
                            hex(flags & 0xFFFFFF), is_comp])
            span = f"ram {ram_lo:#x}-{ram_hi:#x}" if count else "none"
            print(f"overlays {cpu}     count={count} compressed={compressed} "
                  f"rom_bytes={total:#x} {span}")

    print(f"fat entries      {file_count}  named={len(paths)} overlays={len(overlay_files)}")

    dirs = collections.defaultdict(lambda: [0, 0, collections.Counter()])
    magics = collections.Counter()
    sizes = collections.Counter()
    max_end = 0
    with open(os.path.join(args.out_dir, "nds_files.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file_id", "path", "rom_off", "size", "magic"])
        for fid in range(file_count):
            start, end = struct.unpack_from("<II", fat, fid * 8)
            max_end = max(max_end, end)
            data = rom[start:end]
            magic = magic_of(data)
            path = paths.get(fid) or overlay_files.get(fid) or f"(unnamed_{fid})"
            w.writerow([fid, path, hex(start), end - start, magic])
            if fid in overlay_files:
                continue
            top = path.split("/")[0] if "/" in path else "(root)"
            dirs[top][0] += 1
            dirs[top][1] += end - start
            dirs[top][2][magic] += 1
            magics[magic] += 1
            sizes[magic] += end - start

    print(f"last file end    {max_end:#x}")
    print("\ntop-level directories (files, bytes, top magics):")
    for name, (count, size, mags) in sorted(dirs.items(), key=lambda kv: -kv[1][1]):
        top = ", ".join(f"{m}:{c}" for m, c in mags.most_common(4))
        print(f"  {name:24}{count:6}{size:12}  {top}")
    print("\nfile magics (named files, excluding overlays):")
    for magic, count in magics.most_common(20):
        print(f"  {magic:12}{count:6}{sizes[magic]:12}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
