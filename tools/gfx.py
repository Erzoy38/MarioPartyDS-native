#!/usr/bin/env python3
"""DS 2D graphics primitives and a PNG writer using only the standard library."""
import struct
import zlib


def write_png(path, width, height, rgba):
    """rgba: bytes of length width*height*4."""
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def bgr555(value):
    r = value & 31
    g = (value >> 5) & 31
    b = (value >> 10) & 31
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31)


def parse_palette(data):
    """Palette leaf: u16 bytes, u16 colors, then BGR555."""
    colors = struct.unpack_from("<H", data, 2)[0]
    return [bgr555(v) for v in struct.unpack_from(f"<{colors}H", data, 4)]


def parse_tiles(data):
    """Tile leaf: u32 (count << 8 | format). Returns (bpp, list of 64-entry pixel lists)."""
    word = struct.unpack_from("<I", data, 0)[0]
    count = word >> 8
    size = (len(data) - 4) // count
    tiles = []
    if size == 32:
        for i in range(count):
            row = data[4 + i * 32:4 + (i + 1) * 32]
            pixels = []
            for byte in row:
                pixels.append(byte & 15)
                pixels.append(byte >> 4)
            tiles.append(pixels)
        return 4, tiles
    for i in range(count):
        tiles.append(list(data[4 + i * 64:4 + (i + 1) * 64]))
    return 8, tiles


def parse_map(data):
    """Map leaf: u16 bytes, u8 width, u8 height, then u16 entries."""
    width, height = data[2], data[3]
    return width, height, struct.unpack_from(f"<{width * height}H", data, 4)


def render_map(map_data, tile_data, palette, stats=None):
    """Compose a tilemap. Returns (width_px, height_px, rgba bytes)."""
    width, height, entries = parse_map(map_data)
    bpp, tiles = parse_tiles(tile_data)
    out = bytearray(width * 8 * height * 8 * 4)
    stride = width * 8 * 4
    colors = len(palette)
    bad_tile = bad_color = 0
    for my in range(height):
        for mx in range(width):
            entry = entries[my * width + mx]
            index = entry & 0x3FF
            if index >= len(tiles):
                bad_tile += 1
                continue
            hflip, vflip = entry >> 10 & 1, entry >> 11 & 1
            bank = (entry >> 12) * 16 if bpp == 4 else 0
            tile = tiles[index]
            for py in range(8):
                sy = 7 - py if vflip else py
                offset = ((my * 8 + py) * stride) + mx * 8 * 4
                for px in range(8):
                    sx = 7 - px if hflip else px
                    value = tile[sy * 8 + sx]
                    if value == 0:
                        continue
                    color = bank + value
                    if color >= colors:
                        bad_color += 1
                        continue
                    r, g, b = palette[color]
                    pos = offset + px * 4
                    out[pos:pos + 4] = bytes((r, g, b, 255))
    if stats is not None:
        stats["bad_tile"] = bad_tile
        stats["bad_color"] = bad_color
        stats["cells"] = width * height
        stats["tiles"] = len(tiles)
        stats["max_index"] = max(e & 0x3FF for e in entries)
        stats["bpp"] = bpp
    return width * 8, height * 8, bytes(out)


def render_sheet(tile_data, palette, per_row=16, bank=0):
    """Tile sheet in file order, as a check on tile decoding."""
    bpp, tiles = parse_tiles(tile_data)
    rows = (len(tiles) + per_row - 1) // per_row
    width, height = per_row * 8, rows * 8
    out = bytearray(width * height * 4)
    stride = width * 4
    colors = len(palette)
    for i, tile in enumerate(tiles):
        ox, oy = (i % per_row) * 8, (i // per_row) * 8
        for py in range(8):
            for px in range(8):
                value = tile[py * 8 + px]
                color = bank + value
                if value == 0 or color >= colors:
                    continue
                r, g, b = palette[color]
                pos = (oy + py) * stride + (ox + px) * 4
                out[pos:pos + 4] = bytes((r, g, b, 255))
    return width, height, bytes(out)


def render_bitmap16(data, width=256, height=192):
    """Direct-colour bitmap; bit 15 marks an opaque pixel."""
    out = bytearray(width * height * 4)
    values = struct.unpack_from(f"<{width * height}H", data)
    for i, value in enumerate(values):
        if value >> 15:
            r, g, b = bgr555(value)
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    return width, height, bytes(out)
