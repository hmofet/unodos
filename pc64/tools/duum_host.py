#!/usr/bin/env python3
"""Duum host reference - develop/validate the Doom engine on the host (fast,
no QEMU) before porting the same logic to apps/DUUM.PY on UnoDOS.

The rendering math here is deliberately written to port cleanly to
MicroPython: plain lists, struct.unpack, math - no numpy.  The renderer emits
vertical column spans (x, y0, y1, color); on the host we write them to a PPM,
on UnoDOS the same spans become cv.vline() calls.

  python3 tools/duum_host.py map    E1M1 top-down -> shots/duum_map.ppm
  python3 tools/duum_host.py view   E1M1 first-person -> shots/duum_view.ppm
"""
import struct, sys, math, os

WAD = os.path.join(os.path.dirname(__file__), "..", "wads", "DOOM1.WAD")
W, H = 320, 200


# ---- WAD directory + lump access ------------------------------------------
class Wad:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        magic, n, diro = struct.unpack_from("<4sII", self.d, 0)
        assert magic in (b"IWAD", b"PWAD"), magic
        self.dir = []
        for i in range(n):
            off, sz, nm = struct.unpack_from("<II8s", self.d, diro + 16 * i)
            self.dir.append((nm.rstrip(b"\0").decode("latin1"), off, sz))

    def lump(self, name, after=0):
        for i in range(after, len(self.dir)):
            if self.dir[i][0] == name:
                _, off, sz = self.dir[i]
                return self.d[off:off + sz], i
        return None, -1


# ---- map data --------------------------------------------------------------
class Map:
    def __init__(self, wad, name):
        _, mi = wad.lump(name)
        assert mi >= 0, name
        def L(n):
            for j in range(mi + 1, mi + 12):
                if wad.dir[j][0] == n:
                    _, o, s = wad.dir[j]; return wad.d[o:o + s]
            return b""
        def rows(data, fmt, sz):
            return [struct.unpack_from(fmt, data, k) for k in range(0, len(data) - sz + 1, sz)]
        self.verts    = rows(L("VERTEXES"), "<hh", 4)
        self.lines    = rows(L("LINEDEFS"), "<HHHHHHH", 14)
        self.sides    = rows(L("SIDEDEFS"), "<hh8s8s8sH", 30)
        self.segs     = rows(L("SEGS"), "<HHhHHh", 12)
        self.ssectors = rows(L("SSECTORS"), "<HH", 4)
        self.nodes    = rows(L("NODES"), "<hhhh8hHH", 28)
        self.sectors  = rows(L("SECTORS"), "<hh8s8shHH", 26)
        self.things   = rows(L("THINGS"), "<hhHHH", 10)

    def player_start(self):
        for x, y, ang, typ, flags in self.things:
            if typ == 1:
                return x, y, ang
        return 0, 0, 0


# ---- PPM out ---------------------------------------------------------------
def ppm(path, buf):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(bytes(buf))


# ---- top-down map (geometry validation) -----------------------------------
def render_map(m, path):
    xs = [v[0] for v in m.verts]; ys = [v[1] for v in m.verts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    sx = (W - 20) / (maxx - minx); sy = (H - 20) / (maxy - miny)
    s = min(sx, sy)
    def tx(x): return int(10 + (x - minx) * s)
    def ty(y): return int(H - 10 - (y - miny) * s)
    buf = bytearray(W * H * 3)
    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3; buf[i], buf[i+1], buf[i+2] = c
    for v1, v2, flags, typ, tag, sf, sb in m.lines:
        x0, y0 = tx(m.verts[v1][0]), ty(m.verts[v1][1])
        x1, y1 = tx(m.verts[v2][0]), ty(m.verts[v2][1])
        col = (255, 255, 255) if sb == 0xFFFF else (110, 110, 130)  # solid vs 2-sided
        # Bresenham
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        stepx = 1 if x0 < x1 else -1; stepy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            put(x0, y0, col)
            if x0 == x1 and y0 == y1: break
            e2 = 2 * err
            if e2 >= dy: err += dy; x0 += stepx
            if e2 <= dx: err += dx; y0 += stepy
    px, py, pa = m.player_start()
    for ddx in range(-2, 3):
        for ddy in range(-2, 3):
            put(tx(px) + ddx, ty(py) + ddy, (255, 80, 80))
    ppm(path, buf)
    print("wrote", path, "verts", len(m.verts), "lines", len(m.lines),
          "segs", len(m.segs), "ssectors", len(m.ssectors), "nodes", len(m.nodes),
          "sectors", len(m.sectors))
    print("player start", px, py, "angle", pa)


# ---- first-person BSP renderer --------------------------------------------
# Emits vertical spans; on UnoDOS these become cv.vline() calls.  The renderer
# is a classic Doom-style front-to-back BSP walk with per-column open-window
# clipping (ceilclip/floorclip), one-sided solid walls + two-sided portal
# upper/lower steps, and distance+light shading (textures come later).
FOV = math.pi / 2
NEAR = 4.0
EYE = 41


def seg_sectors(m, seg):
    v1, v2, ang, ldi, side, off = seg
    ld = m.lines[ldi]
    front_sd = ld[6] if side else ld[5]
    back_sd  = ld[5] if side else ld[6]
    fs = m.sectors[m.sides[front_sd][5]] if front_sd != 0xFFFF else None
    bs = m.sectors[m.sides[back_sd][5]]  if back_sd  != 0xFFFF else None
    return fs, bs


def point_sector(m, px, py):
    n = len(m.nodes) - 1
    while not (n & 0x8000):
        nd = m.nodes[n]
        nx, ny, ndx, ndy = nd[0], nd[1], nd[2], nd[3]
        side = (px - nx) * ndy - (py - ny) * ndx            # <=0 => front(right)
        n = nd[-2] if side <= 0 else nd[-1]                 # [-2]=right, [-1]=left
    ss = m.ssectors[n & 0x7FFF]
    seg = m.segs[ss[1]]
    fs, _ = seg_sectors(m, seg)
    return fs


def shade(base, dist, light, k=1.0):
    f = (0.72 + 0.28 * (light / 255.0)) * k               # sector light (never dark)
    f *= max(0.6, min(1.0, 1100.0 / (dist + 700.0)))      # gentle distance falloff
    return (min(255, int(base[0] * f)), min(255, int(base[1] * f)), min(255, int(base[2] * f)))


def render_view(m, px, py, pa, path):
    buf = bytearray(W * H * 3)
    ceilc = [0] * W          # top of the still-open window per column (inclusive)
    floorc = [H - 1] * W     # bottom of the open window per column (inclusive)
    cos_a, sin_a = math.cos(pa), math.sin(pa)
    ps = point_sector(m, px, py)
    viewz = (ps[0] if ps else 0) + EYE           # eye = floor height + 41
    scale = (W / 2) / math.tan(FOV / 2)
    hh = H / 2

    def vspan(x, y0, y1, rgb):
        if y1 < y0: return
        if y0 < 0: y0 = 0
        if y1 > H - 1: y1 = H - 1
        r, g, b = rgb
        base = (y0 * W + x) * 3
        for _ in range(y0, y1 + 1):
            buf[base] = r; buf[base + 1] = g; buf[base + 2] = b
            base += W * 3

    CEIL = (84, 88, 104); FLOORC = (120, 100, 74)
    WALL = (200, 188, 170); UPPER = (172, 150, 122); LOWER = (140, 150, 170)

    def draw_seg(seg):
        v1, v2, ang, ldi, side, off = seg
        fs, bs = seg_sectors(m, seg)
        if fs is None: return
        ax, ay = m.verts[v1]; bx, by = m.verts[v2]
        wk = 1.15 if abs(bx - ax) > abs(by - ay) else 0.85    # fake contrast by facing
        # view space: depth = forward, sx = right
        d1x, d1y = ax - px, ay - py
        d2x, d2y = bx - px, by - py
        depth1 = d1x * cos_a + d1y * sin_a
        depth2 = d2x * cos_a + d2y * sin_a
        sx1 = d1y * cos_a - d1x * sin_a
        sx2 = d2y * cos_a - d2x * sin_a
        # near-clip: both behind -> skip; one behind -> clip to NEAR
        if depth1 < NEAR and depth2 < NEAR: return
        if depth1 < NEAR:
            t = (NEAR - depth1) / (depth2 - depth1)
            sx1 += t * (sx2 - sx1); depth1 = NEAR
        elif depth2 < NEAR:
            t = (NEAR - depth2) / (depth1 - depth2)
            sx2 += t * (sx1 - sx2); depth2 = NEAR
        rx1 = (W / 2) + (sx1 / depth1) * scale
        rx2 = (W / 2) + (sx2 / depth2) * scale
        if rx1 >= rx2: return                     # back-face / degenerate
        ix1 = max(0, int(math.ceil(rx1))); ix2 = min(W - 1, int(math.floor(rx2)))
        if ix1 > ix2: return
        inv1, inv2 = 1.0 / depth1, 1.0 / depth2
        fc, ff, flight = fs[1], fs[0], fs[4]
        if bs is not None:
            bc, bf = bs[1], bs[0]
        span = rx2 - rx1
        for x in range(ix1, ix2 + 1):
            if ceilc[x] > floorc[x]:
                continue
            t = (x - rx1) / span
            invd = inv1 + (inv2 - inv1) * t
            dist = 1.0 / invd
            yfc = int(hh - (fc - viewz) * scale * invd)     # front ceil screen y
            yff = int(hh - (ff - viewz) * scale * invd)     # front floor screen y
            ct, cb = ceilc[x], floorc[x]
            # ceiling fill above the front ceiling
            if yfc > ct:
                vspan(x, ct, min(yfc - 1, cb), shade(CEIL, dist, flight))
            # floor fill below the front floor
            if yff < cb:
                vspan(x, max(yff + 1, ct), cb, shade(FLOORC, dist, flight))
            if bs is None:                              # solid wall: full column
                vspan(x, max(yfc, ct), min(yff, cb), shade(WALL, dist, flight, wk))
                ceilc[x] = 1; floorc[x] = 0             # column closed
            else:                                       # two-sided portal
                ybc = int(hh - (bc - viewz) * scale * invd)
                ybf = int(hh - (bf - viewz) * scale * invd)
                newct, newcb = max(ct, yfc), min(cb, yff)
                if bc < fc:                             # upper step
                    vspan(x, max(yfc, ct), min(ybc - 1, cb), shade(UPPER, dist, flight, wk))
                    newct = max(newct, ybc)
                if bf > ff:                             # lower step
                    vspan(x, max(ybf + 1, ct), min(yff, cb), shade(LOWER, dist, flight, wk))
                    newcb = min(newcb, ybf)
                ceilc[x] = newct; floorc[x] = newcb

    def walk(n):
        if n & 0x8000:
            ss = m.ssectors[n & 0x7FFF]
            cnt, first = ss
            for i in range(first, first + cnt):
                draw_seg(m.segs[i])
            return
        nd = m.nodes[n]
        nx, ny, ndx, ndy = nd[0], nd[1], nd[2], nd[3]
        rightchild, leftchild = nd[-2], nd[-1]
        side = ((px - nx) * ndy - (py - ny) * ndx)
        if side <= 0:                                   # player on right(front)
            walk(rightchild); walk(leftchild)
        else:
            walk(leftchild); walk(rightchild)

    import sys as _s; _s.setrecursionlimit(5000)
    walk(len(m.nodes) - 1)
    # sky/remainder fill: any column still open (an opening to nowhere, or an
    # outdoor area) gets a flat sky rather than a black gap.
    SKY = (78, 86, 112)
    for x in range(W):
        if ceilc[x] <= floorc[x]:
            vspan(x, ceilc[x], floorc[x], SKY)
    ppm(path, buf)
    print("wrote", path, "viewz", viewz)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "map"
    wad = Wad(WAD)
    m = Map(wad, "E1M1")
    out = os.path.join(os.path.dirname(__file__), "..", "shots")
    os.makedirs(out, exist_ok=True)
    if cmd == "map":
        render_map(m, os.path.join(out, "duum_map.ppm"))
    elif cmd == "view":
        px, py, pa = m.player_start()
        render_view(m, px, py, math.radians(pa), os.path.join(out, "duum_view.ppm"))
