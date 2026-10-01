#!/usr/bin/env python3
"""Draws packaging/icon.png (256x256): an original sun-over-the-sea icon for the
PC port's desktop entries. No game artwork is used. Run: python3 make_icon.py"""
import math, os, struct, zlib

N = 256
px = bytearray(N * N * 4)

def blend(i, r, g, b, a):
    o = i * 4
    da = px[o + 3] / 255.0
    sa = a / 255.0
    out = sa + da * (1 - sa)
    if out <= 0:
        return
    for k, c in enumerate((r, g, b)):
        px[o + k] = int((c * sa + px[o + k] * da * (1 - sa)) / out)
    px[o + 3] = int(out * 255)

def coverage(x, y, inside, ss=4):
    n = 0
    for sy in range(ss):
        for sx in range(ss):
            if inside(x + (sx + 0.5) / ss, y + (sy + 0.5) / ss):
                n += 1
    return n / (ss * ss)

def rounded(x, y, r=46, m=8):
    cx = min(max(x, m + r), N - m - r)
    cy = min(max(y, m + r), N - m - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and m <= x <= N - m and m <= y <= N - m

sea = 168
for y in range(N):
    for x in range(N):
        c = coverage(x, y, rounded)
        if not c:
            continue
        if y < sea:
            t = y / sea
            col = (int(30 + 96 * t), int(136 + 75 * t), int(229 + 26 * t))
        else:
            t = (y - sea) / (N - sea)
            col = (0, int(172 - 76 * t), int(193 - 33 * t))
        blend(y * N + x, *col, int(255 * c))

sx, sy, sr = 128, 112, 46
for y in range(N):
    for x in range(N):
        if not rounded(x + 0.5, y + 0.5):
            continue
        d = math.hypot(x + 0.5 - sx, y + 0.5 - sy)
        if y + 0.5 > sea:
            continue
        a = math.atan2(y + 0.5 - sy, x + 0.5 - sx)
        ray = 0.5 + 0.5 * math.cos(a * 12)
        if sr < d < sr + 34 * ray ** 3 + 6:
            blend(y * N + x, 255, 214, 64, int(110 * (1 - (d - sr) / 40)))
        c = coverage(x, y, lambda u, v: (u - sx) ** 2 + (v - sy) ** 2 <= sr * sr and v <= sea)
        if c:
            blend(y * N + x, 255, 210, 50, int(255 * c))

for band, (yy, amp, alpha) in enumerate(((sea + 16, 5, 170), (sea + 40, 6, 120), (sea + 64, 7, 80))):
    for x in range(N):
        wy = yy + amp * math.sin(x * 0.07 + band)
        for y in range(int(wy) - 3, int(wy) + 4):
            if 0 <= y < N and rounded(x + 0.5, y + 0.5):
                d = abs(y + 0.5 - wy)
                if d < 3:
                    blend(y * N + x, 255, 255, 255, int(alpha * (1 - d / 3)))

raw = b"".join(b"\0" + bytes(px[y * N * 4:(y + 1) * N * 4]) for y in range(N))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) + \
      chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.png")
open(out, "wb").write(png)
print("wrote", out)
