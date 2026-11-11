"""Draws the iOS app icon (ios/Assets.xcassets/AppIcon.appiconset/AppIcon.png, 1024x1024, no alpha).

The same picture as Android's android/app/src/main/res/drawable/ic_launcher.xml: two stacked screens
on turquoise. iOS rounds the corners itself, so the turquoise fills the whole square.
Usage: python tools/ios_icon.py
"""
import os
import struct
import zlib

SIZE = 1024
VIEW = 108.0  # the Android drawing's viewport
SAMPLES = 3   # per side, for smooth edges

BACKGROUND = (0x30, 0xD0, 0xD8)
# (x0, y0, x1, y1, corner radius, colour) in the 108-unit viewport, drawn in order.
SHAPES = [
    (30, 22, 78, 54, 4, (0x20, 0x20, 0x28)),
    (30, 58, 78, 86, 4, (0x20, 0x20, 0x28)),
    (38, 28, 70, 48, 0, (0xFF, 0xD0, 0x20)),
    (38, 62, 70, 80, 0, (0xFF, 0xFF, 0xFF)),
]


def inside(shape, x, y):
    x0, y0, x1, y1, r, _ = shape
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    if r == 0:
        return True
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def near(shape, x, y):
    """Within a unit of the shape's border (or its rounded corners), where edges need more samples."""
    x0, y0, x1, y1, r, _ = shape
    if not (x0 - 1 <= x <= x1 + 1 and y0 - 1 <= y <= y1 + 1):
        return False
    return min(abs(x - x0), abs(x1 - x), abs(y - y0), abs(y1 - y)) < 1 + r


def draw():
    scale = VIEW / SIZE
    pixels = bytearray()
    offsets = [(i + 0.5) / SAMPLES for i in range(SAMPLES)]
    for py in range(SIZE):
        pixels.append(0)  # PNG filter: none
        for px in range(SIZE):
            # Shapes near this pixel only; far from every edge one sample is enough.
            x_mid, y_mid = (px + 0.5) * scale, (py + 0.5) * scale
            near_edge = any(near(s, x_mid, y_mid) for s in SHAPES)
            points = [(ox, oy) for oy in offsets for ox in offsets] if near_edge else [(0.5, 0.5)]
            total = [0, 0, 0]
            for ox, oy in points:
                x, y = (px + ox) * scale, (py + oy) * scale
                colour = BACKGROUND
                for shape in SHAPES:
                    if inside(shape, x, y):
                        colour = shape[5]
                for c in range(3):
                    total[c] += colour[c]
            pixels.extend(int(round(t / len(points))) for t in total)
    return bytes(pixels)


def write_rgb_png(path, width, height, raw):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))  # 8-bit RGB
        out.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        out.write(chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    folder = os.path.join(root, "ios", "Assets.xcassets", "AppIcon.appiconset")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "AppIcon.png")
    write_rgb_png(path, SIZE, SIZE, draw())
    print(path)


if __name__ == "__main__":
    main()
