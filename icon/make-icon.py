#!/usr/bin/env python3
"""
WebMee launcher icon, built to the Nokia N9 Launcher Icon Guidelines 1.1.

Specs taken from the guidelines rather than guessed:
  bounding box  80x80 px
  icon shape    78x78 px  (the "squircle")
  focal zone    54x54 px  (the glyph never leaves this)
  lighting      ONE light source, from above: light at the top edge, dark at the
                bottom, with a highlight band inside the top of the canvas
  layering      canvas -> highlight -> glyph, glyph carries its own shadow

The glyph is a compass needle. The guidelines' own example sheet includes a
green compass, so the form is in-house style; blue keeps WebMee distinct from
it, and nods at the engine's Safari/WebKit lineage.

No image libraries: everything is rasterised here and written as a PNG by hand,
which keeps the icon reproducible on a machine with only Python. Antialiasing is
8x supersampling, which at this size is indistinguishable from analytic coverage.
"""

import math
import struct
import zlib
import os

SIZE = 80            # bounding box
SHAPE = 78           # squircle
FOCAL = 54           # glyph must fit inside this
SS = 8               # supersample factor
N = SIZE * SS

# Canvas gradient, top (lit) to bottom (shadowed).
TOP = (0x4A, 0xA8, 0xF2)
BOTTOM = (0x0B, 0x4C, 0x9B)

# Needle: red half points "north", pearl-white half opposite -- as on the
# guidelines' own compass, and Safari's.
NEEDLE_RED = (0xE8, 0x43, 0x3A)
NEEDLE_RED_DARK = (0xBE, 0x2F, 0x28)
NEEDLE_WHITE = (0xFF, 0xFF, 0xFF)
NEEDLE_WHITE_DARK = (0xD2, 0xD8, 0xE0)

NEEDLE_ANGLE = 45.0      # red tip to the north-EAST, as Safari's points
NEEDLE_LEN = 25.0        # 2*25 = 50, inside the 54px focal zone
NEEDLE_HALF_W = 7.5


# Superellipse exponent. MEASURED off the stock Nokia browser icon rather than
# guessed: see measure-squircle.py, which solves |u|^n + |v|^n = 1 per boundary
# sample. The reference corner region comes out at n ~= 2.65; an initial guess of
# 4 measured 3.70 and was visibly too square in the corners.
SQUIRCLE_N = 2.78


def squircle(x, y, half):
    """Superellipse: n=2 is a circle, n->inf a square. The N9 shape sits low,
    much closer to a circle than to a rounded rectangle."""
    if half <= 0:
        return False
    return (abs(x / half) ** SQUIRCLE_N + abs(y / half) ** SQUIRCLE_N) <= 1.0


def rotate(px, py, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return px * c - py * s, px * s + py * c


def in_triangle(px, py, a, b, c):
    def sign(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])
    d1 = sign((px, py), a, b)
    d2 = sign((px, py), b, c)
    d3 = sign((px, py), c, a)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def build():
    half_shape = SHAPE / 2.0
    half_focal = FOCAL / 2.0

    # Needle geometry in glyph space, before rotation: a rhombus split across
    # its short axis, each half split again along the long axis to fake the fold.
    tip_n = (0.0, -NEEDLE_LEN)
    tip_s = (0.0, NEEDLE_LEN)
    side_e = (NEEDLE_HALF_W, 0.0)
    side_w = (-NEEDLE_HALF_W, 0.0)

    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(SIZE * SIZE)]

    for sy in range(N):
        # sample centre, in icon coordinates with origin at the middle
        y = (sy + 0.5) / SS - SIZE / 2.0
        for sx in range(N):
            x = (sx + 0.5) / SS - SIZE / 2.0

            r = g = b = 0
            a = 0.0

            if squircle(x, y, half_shape):
                # canvas: vertical gradient, lit from above
                t = (y + half_shape) / (2 * half_shape)
                r, g, b = lerp(TOP, BOTTOM, t)
                a = 1.0

                # highlight: a soft elliptical sheen inside the top edge
                hx, hy = x / (half_shape * 0.85), (y + half_shape * 0.45) / (half_shape * 0.42)
                d = hx * hx + hy * hy
                if d < 1.0:
                    k = (1.0 - d) ** 2 * 0.30
                    r = min(255, int(r + (255 - r) * k))
                    g = min(255, int(g + (255 - g) * k))
                    b = min(255, int(b + (255 - b) * k))

                # glyph shadow: same needle, offset down, softened by distance
                gx, gy = rotate(x, y - 1.4, -NEEDLE_ANGLE)
                if (in_triangle(gx, gy, tip_n, side_e, side_w)
                        or in_triangle(gx, gy, tip_s, side_e, side_w)):
                    r, g, b = int(r * 0.55), int(g * 0.55), int(b * 0.60)

                # glyph: rotate the sample into needle space instead of rotating
                # the needle, so the maths stays axis-aligned
                nx, ny = rotate(x, y, -NEEDLE_ANGLE)
                if in_triangle(nx, ny, tip_n, side_e, side_w):
                    r, g, b = NEEDLE_RED if nx >= 0 else NEEDLE_RED_DARK
                elif in_triangle(nx, ny, tip_s, side_e, side_w):
                    r, g, b = NEEDLE_WHITE if nx < 0 else NEEDLE_WHITE_DARK

            if a > 0.0:
                idx = (sy // SS) * SIZE + (sx // SS)
                cell = acc[idx]
                cell[0] += r
                cell[1] += g
                cell[2] += b
                cell[3] += 1.0

    # Resolve supersamples. Coverage becomes alpha, colour is the mean of the
    # covered samples only -- averaging in the uncovered ones would darken edges.
    total = float(SS * SS)
    rows = []
    for py in range(SIZE):
        row = bytearray(b"\x00")
        for px in range(SIZE):
            r, g, b, cov = acc[py * SIZE + px]
            if cov == 0:
                row += bytes((0, 0, 0, 0))
            else:
                row += bytes((int(round(r / cov)), int(round(g / cov)),
                              int(round(b / cov)), int(round(255 * cov / total))))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path, raw, size):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))  # 6 = RGBA
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


if __name__ == "__main__":
    import sys
    here = os.path.dirname(os.path.abspath(__file__))
    # Optional size, because the launcher wants 80px and the start page wants a
    # bigger one -- and upscaling an 80px icon looks exactly like upscaling an
    # 80px icon. Everything here is drawn from the curve, so any size is sharp.
    if len(sys.argv) > 1:
        # Every measurement below is in the 80px reference grid, so a different
        # canvas has to scale them all -- otherwise the artwork just sits small
        # in a bigger transparent square. SS stays put; it is a sampling rate,
        # not a dimension.
        k = int(sys.argv[1]) / float(SIZE)
        SIZE = int(sys.argv[1])
        SHAPE *= k
        FOCAL *= k
        NEEDLE_LEN *= k
        NEEDLE_HALF_W *= k
        N = SIZE * SS
    out = os.path.join(here, sys.argv[2] if len(sys.argv) > 2
                             else "webmee%d.png" % SIZE)
    raw = build()
    n = write_png(out, raw, SIZE)
    print("wrote %s (%d bytes, %dx%d RGBA)" % (out, n, SIZE, SIZE))
