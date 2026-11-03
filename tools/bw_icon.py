#!/usr/bin/env python3
"""Blood Wake Recomp's icon and Setup's pictures, drawn from code (stdlib only).

    python tools/bw_icon.py ico --out <file.ico>        the program icon (16 to 256 px)
    python tools/bw_icon.py setup --out <dir>           Setup's pictures and the icon
    python tools/bw_icon.py preview --out <dir>         every picture as PNG, to look at

The pictures are our own art: the scene of the project's banner
(docs/images/banner.svg in the public repo: a red wake trailing a small boat
toward the horizon of a night sea, under a moon), redrawn here in its colours
(docs/025/STYLE.md). Nothing comes from the game. They're drawn at build time,
so neither repository holds an image file: the build (CMakeLists.txt) makes
the icon for bloodwake.exe, and installer/build_installer.py makes Setup's.

Small sizes get a simpler drawing (fewer, bolder shapes), as hand-made icons
do. Output is deterministic: the same script gives the same bytes.
"""
import argparse
import math
import os
import struct
import sys
import zlib

# ── Colours (the banner's; docs/025/STYLE.md names them) ──────────────────

def hexc(s):
    s = s.lstrip("#")
    return (int(s[0:2], 16) / 255.0, int(s[2:4], 16) / 255.0, int(s[4:6], 16) / 255.0)


NIGHT_900, NIGHT_800, NIGHT_700 = hexc("#050a13"), hexc("#0a1828"), hexc("#0f2740")
SEA_800, SEA_700, SEA_600, SEA_500 = hexc("#0b2034"), hexc("#0e2e4b"), hexc("#11406a"), hexc("#165684")
SWELL_HI, SWELL_HI2, HORIZON = hexc("#7cc4ff"), hexc("#9fd6ff"), hexc("#3d78a8")
WAKE_BRIGHT, WAKE_MID, WAKE_DEEP, WAKE_DARK = hexc("#ff3b54"), hexc("#d7263d"), hexc("#b3122e"), hexc("#8e0f22")
MOON, MOON_GLOW = hexc("#dcecff"), hexc("#cfe6ff")
BOAT, BOAT_LINE, RIPPLE, WHITE = hexc("#e9f1fa"), hexc("#5a6b80"), hexc("#ffc2ca"), (1.0, 1.0, 1.0)


# ── A small anti-aliased rasteriser ───────────────────────────────────────

class Canvas:
    """Premultiplied RGBA floats. Shapes are polygons in pixel units, filled
    with the non-zero rule: exact coverage across each scanline, SUB samples
    down each pixel."""
    SUB = 8

    def __init__(self, w, h):
        self.w, self.h = w, h
        n = w * h
        self.r, self.g, self.b, self.a = [0.0] * n, [0.0] * n, [0.0] * n, [0.0] * n

    def coverage(self, polys):
        """{row: [coverage per pixel]} for the polygons' union."""
        edges = []
        ymin, ymax = float("inf"), float("-inf")
        for poly in polys:
            n = len(poly)
            for i in range(n):
                x0, y0 = poly[i]
                x1, y1 = poly[(i + 1) % n]
                if y0 == y1:
                    continue
                d = 1
                if y0 > y1:
                    x0, y0, x1, y1, d = x1, y1, x0, y0, -1
                edges.append((y0, y1, x0, (x1 - x0) / (y1 - y0), d))
                ymin, ymax = min(ymin, y0), max(ymax, y1)
        out = {}
        if not edges:
            return out
        w, sub = self.w, self.SUB
        wt = 1.0 / sub
        for j in range(max(0, int(math.floor(ymin))), min(self.h, int(math.ceil(ymax)))):
            row_edges = [e for e in edges if e[0] < j + 1 and e[1] > j]
            if not row_edges:
                continue
            diff = [0.0] * (w + 2)
            part = [0.0] * (w + 1)
            any_span = False
            for k in range(sub):
                ys = j + (k + 0.5) * wt
                xs = sorted((x0 + (ys - y0) * dxdy, d) for y0, y1, x0, dxdy, d in row_edges if y0 <= ys < y1)
                wind = 0
                start = 0.0
                for x, d in xs:
                    before = wind
                    wind += d
                    if before == 0 and wind != 0:
                        start = x
                    elif before != 0 and wind == 0:
                        xa, xb = max(0.0, start), min(float(w), x)
                        if xb <= xa:
                            continue
                        any_span = True
                        ia, ib = int(xa), int(xb)
                        if ia == ib:
                            part[ia] += (xb - xa) * wt
                        else:
                            part[ia] += (ia + 1 - xa) * wt
                            diff[ia + 1] += wt
                            diff[ib] -= wt
                            if ib < w:
                                part[ib] += (xb - ib) * wt
            if not any_span:
                continue
            row = [0.0] * w
            acc = 0.0
            for i in range(w):
                acc += diff[i]
                c = acc + part[i]
                row[i] = 1.0 if c > 1.0 else (0.0 if c < 1e-6 else c)
            out[j] = row
        return out

    def fill(self, polys, paint, opacity=1.0):
        """paint: (r, g, b) or (r, g, b, a), or a function (x, y) -> (r, g, b, a)."""
        cov = self.coverage(polys)
        solid = not callable(paint)
        if solid:
            cr, cg, cb = paint[0], paint[1], paint[2]
            ca = (paint[3] if len(paint) > 3 else 1.0) * opacity
        R, G, B, A, w = self.r, self.g, self.b, self.a, self.w
        for j, row in cov.items():
            base = j * w
            for i in range(w):
                c = row[i]
                if c <= 0.0:
                    continue
                if solid:
                    sr, sg, sb, sa = cr, cg, cb, ca * c
                else:
                    sr, sg, sb, pa = paint(i + 0.5, j + 0.5)
                    sa = pa * opacity * c
                if sa <= 0.0:
                    continue
                k = base + i
                inv = 1.0 - sa
                R[k] = sr * sa + R[k] * inv
                G[k] = sg * sa + G[k] * inv
                B[k] = sb * sa + B[k] * inv
                A[k] = sa + A[k] * inv

    def mask(self, polys):
        """Keep only what's inside the polygons (the icon's rounded tile)."""
        cov = self.coverage(polys)
        w = self.w
        for j in range(self.h):
            row = cov.get(j)
            for i in range(w):
                c = row[i] if row else 0.0
                if c < 1.0:
                    k = j * w + i
                    self.r[k] *= c
                    self.g[k] *= c
                    self.b[k] *= c
                    self.a[k] *= c

    def rgba8(self):
        """Straight (not premultiplied) 8-bit RGBA rows, top first."""
        out = bytearray(self.w * self.h * 4)
        for k in range(self.w * self.h):
            a = self.a[k]
            if a > 0.0:
                out[k * 4] = min(255, int(self.r[k] / a * 255.0 + 0.5))
                out[k * 4 + 1] = min(255, int(self.g[k] / a * 255.0 + 0.5))
                out[k * 4 + 2] = min(255, int(self.b[k] / a * 255.0 + 0.5))
                out[k * 4 + 3] = min(255, int(a * 255.0 + 0.5))
        return bytes(out)


# ── Shapes, in design units (scaled by s) ─────────────────────────────────

def orient(poly):
    """Every polygon counter-clockwise, so overlapping ones unite under non-zero."""
    a = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % len(poly)]
        a += x0 * y1 - x1 * y0
    return poly if a >= 0 else poly[::-1]


class Path:
    """A path like SVG's (M, L, H, V, C, S, Q, Z), flattened to polygons."""

    def __init__(self, s=1.0, dx=0.0, dy=0.0):
        self.s, self.dx, self.dy = s, dx, dy
        self.polys, self.cur, self.pt, self.last_c2 = [], None, (0.0, 0.0), None

    def _p(self, x, y):
        return (x * self.s + self.dx, y * self.s + self.dy)

    def M(self, x, y):
        self.cur = [self._p(x, y)]
        self.polys.append(self.cur)
        self.pt, self.last_c2 = (x, y), None
        return self

    def L(self, x, y):
        self.cur.append(self._p(x, y))
        self.pt, self.last_c2 = (x, y), None
        return self

    def H(self, x):
        return self.L(x, self.pt[1])

    def V(self, y):
        return self.L(self.pt[0], y)

    def C(self, x1, y1, x2, y2, x, y, n=24):
        x0, y0 = self.pt
        for i in range(1, n + 1):
            t = i / float(n)
            u = 1 - t
            px = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x
            py = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y
            self.cur.append(self._p(px, py))
        self.pt, self.last_c2 = (x, y), (x2, y2)
        return self

    def S(self, x2, y2, x, y):
        x0, y0 = self.pt
        if self.last_c2:
            x1, y1 = 2 * x0 - self.last_c2[0], 2 * y0 - self.last_c2[1]
        else:
            x1, y1 = x0, y0
        return self.C(x1, y1, x2, y2, x, y)

    def Q(self, x1, y1, x, y, n=16):
        x0, y0 = self.pt
        for i in range(1, n + 1):
            t = i / float(n)
            u = 1 - t
            self.cur.append(self._p(u * u * x0 + 2 * u * t * x1 + t * t * x, u * u * y0 + 2 * u * t * y1 + t * t * y))
        self.pt, self.last_c2 = (x, y), None
        return self

    def Z(self):
        return self

    def fill_polys(self):
        return [orient(p) for p in self.polys if len(p) > 2]

    def lines(self):
        return [p for p in self.polys if len(p) > 1]


def circle(cx, cy, r, s=1.0, n=None):
    n = n or max(24, int(r * s * 3))
    return [orient([((cx + r * math.cos(2 * math.pi * i / n)) * s, (cy + r * math.sin(2 * math.pi * i / n)) * s)
                    for i in range(n)])]


def rounded_rect(x0, y0, x1, y1, rad, s=1.0, n=12):
    pts = []
    for cx, cy, a0 in ((x1 - rad, y0 + rad, -90), (x1 - rad, y1 - rad, 0), (x0 + rad, y1 - rad, 90), (x0 + rad, y0 + rad, 180)):
        for i in range(n + 1):
            a = math.radians(a0 + 90.0 * i / n)
            pts.append(((cx + rad * math.cos(a)) * s, (cy + rad * math.sin(a)) * s))
    return [orient(pts)]


def rect(x0, y0, x1, y1, s=1.0):
    return [orient([(x0 * s, y0 * s), (x1 * s, y0 * s), (x1 * s, y1 * s), (x0 * s, y1 * s)])]


def stroke(lines, width, dash=None):
    """Round-capped strokes of polylines (pixel units) as polygons."""
    out = []
    hw = width / 2.0
    for line in lines:
        segs = list(zip(line[:-1], line[1:]))
        if dash:
            segs = dashed(segs, dash)
        for (x0, y0), (x1, y1) in segs:
            dx, dy = x1 - x0, y1 - y0
            L = math.hypot(dx, dy)
            if L < 1e-9:
                continue
            nx, ny = -dy / L * hw, dx / L * hw
            out.append(orient([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]))
            out.extend(circle(x1, y1, hw, 1.0, max(8, int(hw * 4))))
        if segs:
            out.extend(circle(segs[0][0][0], segs[0][0][1], hw, 1.0, max(8, int(hw * 4))))
    return out


def dashed(segs, pattern):
    out, k, left, on = [], 0, pattern[0], True
    for (x0, y0), (x1, y1) in segs:
        L = math.hypot(x1 - x0, y1 - y0)
        t = 0.0
        while t < L:
            step = min(left, L - t)
            if on:
                a, b = t / L, (t + step) / L
                out.append(((x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)))
            t += step
            left -= step
            if left <= 1e-9:
                k = (k + 1) % len(pattern)
                left, on = pattern[k], not on
    return out


# ── Paints ────────────────────────────────────────────────────────────────

def lerp_stops(stops, t):
    t = max(0.0, min(1.0, t))
    for (t0, c0, a0), (t1, c1, a1) in zip(stops[:-1], stops[1:]):
        if t <= t1:
            f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
            return (c0[0] + (c1[0] - c0[0]) * f, c0[1] + (c1[1] - c0[1]) * f,
                    c0[2] + (c1[2] - c0[2]) * f, a0 + (a1 - a0) * f)
    c, a = stops[-1][1], stops[-1][2]
    return (c[0], c[1], c[2], a)


def vgrad(y0, y1, stops):
    def paint(x, y):
        return lerp_stops(stops, (y - y0) / float(y1 - y0))
    return paint


def rgrad(cx, cy, r, stops, sy=1.0):
    def paint(x, y):
        return lerp_stops(stops, math.hypot(x - cx, (y - cy) / sy) / r)
    return paint


# ── The icon (design units: a 256 square) ─────────────────────────────────

def draw_icon(n):
    """The icon at n x n pixels. Below 32 px: the tile, the sky, the horizon, the
    wake and the boat only, bolder; 32 and up add the moon and the swell; 64 and
    up add the foam and the ripples."""
    cv = Canvas(n, n)
    s = n / 256.0
    small, tiny = n < 32, n < 24
    m = 0.0 if tiny else 6.0                       # the tile's margin
    rad = 52.0 if tiny else 46.0
    H = 142.0                                      # the horizon
    tile = rounded_rect(m, m, 256 - m, 256 - m, rad, s)
    cv.fill(rect(0, 0, 256, 256, s), vgrad(0, H * s, [(0, NIGHT_900, 1), (0.6, NIGHT_800, 1), (1, NIGHT_700, 1)]))
    cv.fill(rect(0, H, 256, 256, s), SEA_800)
    # the red glow on the horizon behind the boat
    cv.fill(rect(0, 0, 256, 256, s), rgrad(128 * s, H * s, 150 * s, [(0, WAKE_DEEP, 0.55 if small else 0.42), (1, WAKE_DEEP, 0)], 0.62))
    if not small:
        cv.fill(circle(188, 62, 54, s), rgrad(188 * s, 62 * s, 54 * s, [(0, MOON_GLOW, 0.24), (1, MOON_GLOW, 0)]))
        cv.fill(circle(188, 62, 15, s), MOON, 0.9)
        # the swell, as in the banner: three bands toward the viewer
        p = Path(s).M(0, H + 10).C(40, H + 2, 80, H + 18, 128, H + 10).S(216, H + 2, 256, H + 12).L(256, 256).L(0, 256)
        cv.fill(p.fill_polys(), SEA_700, 0.9)
        p = Path(s).M(0, H + 44).C(48, H + 30, 90, H + 58, 140, H + 44).S(220, H + 34, 256, H + 42).L(256, 256).L(0, 256)
        cv.fill(p.fill_polys(), SEA_600, 0.85)
        p = Path(s).M(0, H + 84).C(56, H + 68, 100, H + 98, 152, H + 82).S(226, H + 70, 256, H + 80).L(256, 256).L(0, 256)
        cv.fill(p.fill_polys(), SEA_500, 0.85)
        if n >= 48:
            hl = Path(s).M(0, H + 44).C(48, H + 30, 90, H + 58, 140, H + 44).S(220, H + 34, 256, H + 42)
            cv.fill(stroke(hl.lines(), max(1.0, 2.2 * s)), SWELL_HI, 0.3)
            hl = Path(s).M(0, H + 84).C(56, H + 68, 100, H + 98, 152, H + 82).S(226, H + 70, 256, H + 80)
            cv.fill(stroke(hl.lines(), max(1.0, 2.2 * s)), SWELL_HI2, 0.32)
    else:
        p = Path(s).M(0, H + 40).C(60, H + 28, 120, H + 52, 256, H + 36).L(256, 256).L(0, 256)
        cv.fill(p.fill_polys(), SEA_600, 0.7)
    hz_w = max(1.0, 2.0 * s) if not tiny else 1.0
    cv.fill(stroke([[(0, H * s), (256 * s, H * s)]], hz_w), HORIZON, 0.55 if not small else 0.7)
    # the wake: a widening red V from the boat's stern to the bottom edge
    top = H + 22.0
    if small:
        lx0, rx0, lx1, rx1 = 118.0, 138.0, 22.0, 234.0
        stops = [(0, WAKE_BRIGHT, 1.0), (0.55, WAKE_MID, 0.9), (1, WAKE_DEEP, 0.55)]
        op = 1.0
    else:
        lx0, rx0, lx1, rx1 = 121.0, 135.0, 30.0, 226.0
        stops = [(0, WAKE_BRIGHT, 0.97), (0.45, WAKE_MID, 0.62), (1, WAKE_DARK, 0.08)]
        op = 0.9
    wake = Path(s).M(lx0, top).C(lx0 - 14, top + 30, lx1 + 40, 230, lx1, 262).L(rx1, 262).C(rx1 - 40, 230, rx0 + 14, top + 30, rx0, top)
    cv.fill(wake.fill_polys(), vgrad(top * s, 256 * s, stops), op)
    if n >= 64:
        for side in (-1, 1):
            x0 = lx0 if side < 0 else rx0
            x1 = lx1 if side < 0 else rx1
            fl = Path(s).M(x0, top + 2).C(x0 + side * 14, top + 32, x1 - side * 38, 230, x1 + side * 2, 262)
            cv.fill(stroke(fl.lines(), 3.2 * s, [16 * s, 6 * s, 7 * s, 6 * s]),
                    vgrad(top * s, 256 * s, [(0, WHITE, 0.85), (1, WHITE, 0.05)]))
        for i, (y, hw) in enumerate(((top + 22, 14), (top + 46, 26), (top + 72, 40))):
            rp = Path(s).M(128 - hw, y).Q(128, y - 7 - i * 2, 128 + hw, y)
            cv.fill(stroke(rp.lines(), 2.6 * s), RIPPLE, 0.45)
    # the boat, seen from behind, heading for the horizon
    if small:
        bw, bt, bb = 17.0, H - 20.0, H + 26.0
    else:
        bw, bt, bb = 15.0, H - 13.0, H + 23.0
    boat = Path(s).M(128, bt).C(128 + bw * 0.6, bt + 10, 128 + bw, bb - 16, 128 + bw * 0.85, bb).L(128 - bw * 0.85, bb).C(128 - bw, bb - 16, 128 - bw * 0.6, bt + 10, 128, bt)
    cv.fill(boat.fill_polys(), BOAT)
    if not small:
        cv.fill(stroke([[((128 - bw * 0.55) * s, (bb - 13) * s), ((128 + bw * 0.55) * s, (bb - 13) * s)]], max(1.0, 3.4 * s)), BOAT_LINE)
    cv.mask(tile)
    # a fine light edge, so the dark tile stands out on a dark background
    if n >= 24:
        edge = rounded_rect(m + 0.5 / s, m + 0.5 / s, 256 - m - 0.5 / s, 256 - m - 0.5 / s, rad - 0.5 / s, s)
        inner = rounded_rect(m + 1.6 / s, m + 1.6 / s, 256 - m - 1.6 / s, 256 - m - 1.6 / s, rad - 1.6 / s, s)
        ring = edge + [p[::-1] for p in inner]
        cv.fill(ring, HORIZON, 0.45)
    return cv


# ── Setup's side picture (design units: 164 x 314, Inno's aspect) ─────────

def draw_side(w, h):
    cv = Canvas(w, h)
    s = w / 164.0
    H = 182.0
    cv.fill(rect(0, 0, 164, 314, s), vgrad(0, H * s, [(0, NIGHT_900, 1), (0.6, NIGHT_800, 1), (1, NIGHT_700, 1)]))
    cv.fill(rect(0, H, 164, 314, s), SEA_800)
    cv.fill(rect(0, 0, 164, 314, s), rgrad(82 * s, H * s, 120 * s, [(0, WAKE_DEEP, 0.36), (1, WAKE_DEEP, 0)], 0.7))
    cv.fill(circle(116, 70, 46, s), rgrad(116 * s, 70 * s, 46 * s, [(0, MOON_GLOW, 0.22), (1, MOON_GLOW, 0)]))
    cv.fill(circle(116, 70, 8, s), MOON, 0.85)
    bands = (((H + 8), SEA_700, 0.9, None), ((H + 38), SEA_600, 0.85, SWELL_HI), ((H + 80), SEA_500, 0.85, SWELL_HI2))
    for i, (y, col, op, hi) in enumerate(bands):
        p = Path(s).M(0, y).C(30, y - 10, 60, y + 10, 92, y).S(146, y - 8, 164, y + 2).L(164, 314).L(0, 314)
        cv.fill(p.fill_polys(), col, op)
        if hi:
            ln = Path(s).M(0, y).C(30, y - 10, 60, y + 10, 92, y).S(146, y - 8, 164, y + 2)
            cv.fill(stroke(ln.lines(), max(1.0, 1.3 * s)), hi, 0.3)
    cv.fill(stroke([[(0, H * s), (164 * s, H * s)]], max(1.0, 1.0 * s)), HORIZON, 0.5)
    top = H + 14.0
    lx0, rx0, lx1, rx1 = 77.5, 86.5, 10.0, 154.0
    wake = Path(s).M(lx0, top).C(lx0 - 10, top + 24, lx1 + 30, 280, lx1, 318).L(rx1, 318).C(rx1 - 30, 280, rx0 + 10, top + 24, rx0, top)
    cv.fill(wake.fill_polys(), vgrad(top * s, 314 * s, [(0, WAKE_BRIGHT, 0.97), (0.45, WAKE_MID, 0.6), (1, WAKE_DARK, 0.05)]), 0.85)
    for side in (-1, 1):
        x0 = lx0 if side < 0 else rx0
        x1 = lx1 if side < 0 else rx1
        fl = Path(s).M(x0, top + 1).C(x0 + side * 10, top + 26, x1 - side * 28, 280, x1 + side * 2, 318)
        cv.fill(stroke(fl.lines(), 1.6 * s, [9 * s, 3.5 * s, 4 * s, 3.5 * s]), vgrad(top * s, 314 * s, [(0, WHITE, 0.85), (1, WHITE, 0.04)]))
    for i, (y, hw) in enumerate(((top + 14, 8), (top + 30, 15), (top + 50, 24), (top + 76, 34))):
        rp = Path(s).M(82 - hw, y).Q(82, y - 4 - i * 1.5, 82 + hw, y)
        cv.fill(stroke(rp.lines(), 1.4 * s), RIPPLE, 0.42)
    bw, bt, bb = 9.0, H - 8.0, H + 15.0
    boat = Path(s).M(82, bt).C(82 + bw * 0.6, bt + 6, 82 + bw, bb - 10, 82 + bw * 0.85, bb).L(82 - bw * 0.85, bb).C(82 - bw, bb - 10, 82 - bw * 0.6, bt + 6, 82, bt)
    cv.fill(boat.fill_polys(), BOAT)
    cv.fill(stroke([[((82 - bw * 0.55) * s, (bb - 8) * s), ((82 + bw * 0.55) * s, (bb - 8) * s)]], max(1.0, 2.0 * s)), BOAT_LINE)
    return cv


# ── Files ─────────────────────────────────────────────────────────────────

def png(w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def ico_dib(n, rgba):
    """An icon image as a 32-bit DIB (bottom-up BGRA plus the 1-bit AND mask)."""
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = bytearray()
    for y in range(n - 1, -1, -1):
        row = rgba[y * n * 4:(y + 1) * n * 4]
        for x in range(n):
            r, g, b, a = row[x * 4:x * 4 + 4]
            px += bytes((b, g, r, a))
    mask_stride = ((n + 31) // 32) * 4
    mask = bytearray()
    for y in range(n - 1, -1, -1):
        bits = bytearray(mask_stride)
        for x in range(n):
            if rgba[(y * n + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return hdr + bytes(px) + bytes(mask)


ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)


def ico(sizes=ICO_SIZES):
    """A multi-size .ico: DIBs up to 64 px, PNG at 256 (as Windows' own icons)."""
    images = []
    for n in sizes:
        rgba = draw_icon(n).rgba8()
        images.append((n, png(n, n, rgba) if n >= 256 else ico_dib(n, rgba)))
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    for n, data in images:
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    return out + b"".join(d for _, d in images)


SMALL_SIZES = (58, 77, 97, 124, 159)                       # Inno's small picture at 100-250 %
SIDE_SIZES = ((202, 386), (269, 515), (336, 643), (430, 824), (534, 1022))


def write(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def write_setup(out):
    """Setup's pictures: returns the comma-separated lists Inno takes."""
    write(os.path.join(out, "bloodwake.ico"), ico())
    small, side = [], []
    for n in SMALL_SIZES:
        p = os.path.join(out, "setup_small_%d.png" % n)
        write(p, png(n, n, draw_icon(n).rgba8()))
        small.append(p)
    for w, h in SIDE_SIZES:
        p = os.path.join(out, "setup_side_%dx%d.png" % (w, h))
        write(p, png(w, h, draw_side(w, h).rgba8()))
        side.append(p)
    return small, side


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("ico", "setup", "preview"))
    ap.add_argument("--out", required=True, help="the .ico file (ico), or a folder (setup, preview)")
    a = ap.parse_args(argv)
    if a.what == "ico":
        write(a.out, ico())
        print("icon written: %s (%s px)" % (a.out, ", ".join(map(str, ICO_SIZES))))
    elif a.what == "setup":
        small, side = write_setup(a.out)
        print("Setup's pictures written to %s: %d small, %d side, and bloodwake.ico" % (a.out, len(small), len(side)))
    else:
        for n in ICO_SIZES + (128,):
            write(os.path.join(a.out, "icon_%d.png" % n), png(n, n, draw_icon(n).rgba8()))
        w, h = SIDE_SIZES[1]
        write(os.path.join(a.out, "side_%dx%d.png" % (w, h)), png(w, h, draw_side(w, h).rgba8()))
        write(os.path.join(a.out, "sheet.png"), contact_sheet())
        print("previews written to %s" % a.out)
    return 0


def contact_sheet(sizes=(16, 20, 24, 32, 48, 64), zoom=4, pad=8):
    """Each small size enlarged (nearest pixel) and at its real size, on a light
    row and a dark row (Windows' light and dark taskbar and Start colours)."""
    cells = [(n, draw_icon(n).rgba8()) for n in sizes]
    w = pad + sum(n * zoom + pad + n + pad for n in sizes)
    rowh = max(sizes) * zoom + 2 * pad
    h = rowh * 2
    out = bytearray(w * h * 4)
    for row, bg in enumerate(((0xf3, 0xf3, 0xf3), (0x20, 0x20, 0x20))):
        for y in range(row * rowh, (row + 1) * rowh):
            for x in range(w):
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(bg + (255,))
        x0 = pad
        for n, rgba in cells:
            for z, dx in ((zoom, x0), (1, x0 + n * zoom + pad)):
                for y in range(n * z):
                    for x in range(n * z):
                        sy, sx = y // z, x // z
                        r, g, b, a = rgba[(sy * n + sx) * 4:(sy * n + sx) * 4 + 4]
                        k = ((row * rowh + pad + y) * w + dx + x) * 4
                        f = a / 255.0
                        for c, v in enumerate((r, g, b)):
                            out[k + c] = int(v * f + out[k + c] * (1 - f) + 0.5)
            x0 += n * zoom + pad + n + pad
    return png(w, h, bytes(out))


if __name__ == "__main__":
    sys.exit(main())
