#!/usr/bin/env python3
"""
Measure the real N9 launcher squircle instead of guessing its exponent.

Decodes a stock icon, takes the alpha silhouette, and for each boundary sample
solves |u|^n + |v|^n = 1 for n (u,v normalised to the shape's own half-extent).
A circle gives n=2, a square n=inf; the N9 shape sits in between and the
distribution of n tells us where.

Corners are what actually distinguish the curves, so the fit is reported both
overall and restricted to the diagonal region where the exponent dominates.
"""

import struct
import zlib
import sys
import math


def decode_png(path):
    """Minimal 8-bit RGBA decoder with all five filter types."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    w = h = 0
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        tag = d[pos + 4:pos + 8]
        data = d[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", data[:10])[:4]
            assert bd == 8 and ct == 6, "expected 8-bit RGBA, got bd=%d ct=%d" % (bd, ct)
        elif tag == b"IDAT":
            idat += data
        pos += 12 + ln

    raw = zlib.decompress(idat)
    bpp, stride = 4, w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def measure(path, label):
    w, h, px = decode_png(path)

    def alpha(x, y):
        return px[(y * w + x) * 4 + 3]

    # Shape extent from the alpha silhouette, at a mid threshold so antialiased
    # edges do not inflate it.
    xs = [x for y in range(h) for x in range(w) if alpha(x, y) > 128]
    ys = [y for y in range(h) for x in range(w) if alpha(x, y) > 128]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sw, sh = x1 - x0 + 1, y1 - y0 + 1
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    ax, ay = sw / 2.0, sh / 2.0

    print("%s: %dx%d canvas, shape %dx%d at (%d,%d)-(%d,%d)"
          % (label, w, h, sw, sh, x0, y0, x1, y1))

    # Walk each row, take the outermost lit pixel, solve for n.
    all_n, corner_n = [], []
    for y in range(y0, y1 + 1):
        row = [x for x in range(w) if alpha(x, y) > 128]
        if not row:
            continue
        v = (y + 0.5 - cy) / ay
        for x in (min(row), max(row)):
            u = (x + 0.5 - cx) / ax
            au, av = abs(u), abs(v)
            if au < 0.05 or av < 0.05 or au > 0.999 or av > 0.999:
                continue      # axis-crossings carry no information about n
            # solve |u|^n + |v|^n = 1
            lo, hi = 1.2, 12.0
            for _ in range(60):
                mid = (lo + hi) / 2.0
                if au ** mid + av ** mid > 1.0:
                    lo = mid
                else:
                    hi = mid
            n = (lo + hi) / 2.0
            all_n.append(n)
            # the corner region is where the exponent actually shows
            if 0.45 < av < 0.95:
                corner_n.append(n)

    def stats(vals, name):
        if not vals:
            print("   %s: no samples" % name)
            return
        vals = sorted(vals)
        med = vals[len(vals) // 2]
        mean = sum(vals) / len(vals)
        print("   %-22s n median %.2f  mean %.2f  (min %.2f max %.2f, %d samples)"
              % (name, med, mean, vals[0], vals[-1], len(vals)))

    stats(all_n, "whole outline")
    stats(corner_n, "corner region only")
    return sw, sh


if __name__ == "__main__":
    for path, label in [(sys.argv[1], "reference"), (sys.argv[2], "ours")]:
        measure(path, label)
        print()
