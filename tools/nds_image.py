#!/usr/bin/env python3
"""Address space and labels shared by the disassembly tools.

Overlays overlap in RAM, so an address in the overlay area only has a meaning
together with an overlay id. Space(ov=N) selects one.
"""
import csv
import os
import struct

UNPACKED = "unpacked"
ARM9_BASE = 0x02000000
ARM7_BASE = 0x02380000


class Segment:
    def __init__(self, name, base, data):
        self.name = name
        self.base = base
        self.data = data

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.data)


def load_overlay_table(path="nds_overlays.csv"):
    with open(path, newline="") as f:
        return {int(r["id"]): r for r in csv.DictReader(f) if r["cpu"] == "arm9"}


def arm9_module_params(image):
    idx = image.find(struct.pack("<II", 0xDEC00621, 0x2106C0DE))
    if idx < 0x1C:
        raise ValueError("ARM9 module params not found")
    names = ("autoload_list", "autoload_list_end", "autoload_start",
             "static_bss_start", "static_bss_end")
    return dict(zip(names, struct.unpack_from("<5I", image, idx - 0x1C)))


def arm9_segments(image):
    """Split the unpacked ARM9 image into its runtime segments."""
    params = arm9_module_params(image)
    src = params["autoload_start"] - ARM9_BASE
    segments = [Segment("arm9", ARM9_BASE, image[:src])]
    for pos in range(params["autoload_list"] - ARM9_BASE,
                     params["autoload_list_end"] - ARM9_BASE, 12):
        dest, size, _bss = struct.unpack_from("<3I", image, pos)
        name = "itcm" if dest < ARM9_BASE else "dtcm"
        segments.append(Segment(name, dest, image[src:src + size]))
        src += size
    return segments


class Space:
    """Readable memory for one CPU, with at most one overlay mapped."""

    def __init__(self, ov=None, arm7=False, root=UNPACKED):
        self.ov = ov
        self.arm7 = arm7
        self.segments = []
        if arm7:
            with open(os.path.join(root, "arm7.bin"), "rb") as f:
                self.segments.append(Segment("arm7", ARM7_BASE, f.read()))
        else:
            with open(os.path.join(root, "arm9.bin"), "rb") as f:
                self.segments.extend(arm9_segments(f.read()))
            if ov is not None:
                table = load_overlay_table()
                with open(os.path.join(root, "overlay", f"ov{ov:03}.bin"), "rb") as f:
                    self.segments.append(
                        Segment(f"ov{ov:03}", int(table[ov]["ram"], 16), f.read()))
        self.labels = load_labels(ov)

    def segment_at(self, addr):
        # The overlay comes last but must win over nothing else: segments do not overlap.
        for seg in self.segments:
            if seg.contains(addr):
                return seg
        return None

    def read(self, addr, count):
        seg = self.segment_at(addr)
        if seg is None or addr + count > seg.base + len(seg.data):
            return None
        off = addr - seg.base
        return seg.data[off:off + count]

    def u16(self, addr):
        data = self.read(addr, 2)
        return None if data is None else struct.unpack("<H", data)[0]

    def u32(self, addr):
        data = self.read(addr, 4)
        return None if data is None else struct.unpack("<I", data)[0]

    def label(self, addr):
        return self.labels.get(addr) or self.labels.get(addr & ~1)


def parse_addr(text):
    return int(text, 16)


def load_labels(ov=None, path="labels.csv"):
    """Return {addr: (name, comment)}. Overlay labels apply only to that overlay."""
    labels = {}
    if not os.path.exists(path):
        return labels
    prefix = None if ov is None else f"ov{ov:03}:"
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            addr = row["addr"].strip()
            if addr.startswith("ov"):
                if prefix is None or not addr.startswith(prefix):
                    continue
                addr = addr[len(prefix):]
            labels[parse_addr(addr)] = (row["name"], row.get("comment") or "")
    return labels


def all_code_images(root=UNPACKED):
    """Yield (tag, base, data) for the ARM9 program and every overlay."""
    with open(os.path.join(root, "arm9.bin"), "rb") as f:
        for seg in arm9_segments(f.read()):
            yield seg.name, seg.base, seg.data
    for ov_id, row in sorted(load_overlay_table().items()):
        with open(os.path.join(root, "overlay", f"ov{ov_id:03}.bin"), "rb") as f:
            yield f"ov{ov_id:03}", int(row["ram"], 16), f.read()
