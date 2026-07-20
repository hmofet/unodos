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
VER = 1.5             # vertical exaggeration: walls were too short vs sky/floor


# ---- textures: palette, patches, TEXTUREx composition ---------------------
def load_palette(wad):
    d, _ = wad.lump("PLAYPAL")
    return [(d[i*3], d[i*3+1], d[i*3+2]) for i in range(256)]


def load_pnames(wad):
    d, _ = wad.lump("PNAMES")
    n = struct.unpack_from("<I", d, 0)[0]
    return [d[4+i*8:4+i*8+8].split(b"\0")[0].decode("latin1").upper() for i in range(n)]


def parse_textures(wad, lumpname, pnames, out):
    d, _ = wad.lump(lumpname)
    if d is None:
        return
    count = struct.unpack_from("<I", d, 0)[0]
    offs = struct.unpack_from("<%dI" % count, d, 4)
    for o in offs:
        name = d[o:o+8].split(b"\0")[0].decode("latin1").upper()
        w, h = struct.unpack_from("<HH", d, o+12)
        pc = struct.unpack_from("<H", d, o+20)[0]
        patches = []
        for p in range(pc):
            ox, oy, pidx = struct.unpack_from("<hhH", d, o+22+p*10)
            if pidx < len(pnames):
                patches.append((ox, oy, pnames[pidx]))
        out[name] = (w, h, patches)


def decode_patch(wad, name):
    d, _ = wad.lump(name)
    if d is None:
        return None
    w, h, lo, to = struct.unpack_from("<HHhh", d, 0)
    colofs = struct.unpack_from("<%dI" % w, d, 8)
    cols = []
    for c in range(w):
        posts = []
        p = colofs[c]
        while p < len(d) and d[p] != 0xFF:
            top = d[p]; length = d[p+1]
            pix = d[p+3:p+3+length]
            posts.append((top, pix))
            p += length + 4
        cols.append(posts)
    return w, h, cols


def compose_texture(wad, texdef):
    """Return (w, h, grid) where grid[x*h + y] = palette index (-1 transparent)."""
    w, h, patches = texdef
    grid = bytearray([0]) * (w * h)      # 0 = default fill; good enough (no masking on walls)
    for (ox, oy, pname) in patches:
        pat = decode_patch(wad, pname)
        if pat is None:
            continue
        pw, ph, cols = pat
        for cx in range(pw):
            dx = ox + cx
            if dx < 0 or dx >= w:
                continue
            base = dx * h
            for (top, pix) in cols[cx]:
                for i in range(len(pix)):
                    dy = oy + top + i
                    if 0 <= dy < h:
                        grid[base + dy] = pix[i]
    return w, h, grid


class Textures:
    def __init__(self, wad):
        self.wad = wad
        self.pal = load_palette(wad)
        pn = load_pnames(wad)
        self.defs = {}
        parse_textures(wad, "TEXTURE1", pn, self.defs)
        parse_textures(wad, "TEXTURE2", pn, self.defs)
        self.cache = {}

    def get(self, name):
        name = name.upper()
        if name in self.cache:
            return self.cache[name]
        td = self.defs.get(name)
        t = compose_texture(self.wad, td) if td else None
        self.cache[name] = t
        return t


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


def render_view(m, px, py, pa, path, tx):
    buf = bytearray(W * H * 3)
    ceilc = [0] * W          # top of the still-open window per column (inclusive)
    floorc = [H - 1] * W     # bottom of the open window per column (inclusive)
    cos_a, sin_a = math.cos(pa), math.sin(pa)
    ps = point_sector(m, px, py)
    viewz = (ps[0] if ps else 0) + EYE           # eye = floor height + 41
    scale = (W / 2) / math.tan(FOV / 2)
    vscale = scale * VER          # taller walls: vertical exaggeration/aspect
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
    pal = tx.pal

    def texname(b):
        s = b.split(b"\0")[0].decode("latin1")
        return s if (s and s != "-") else None

    def wall_col(x, ytop, ybot, ct, cb, tex, tc, v0, dv, f, fb):
        """Draw a textured wall column clipped to the open window [ct,cb]."""
        y0 = ytop if ytop > ct else ct
        y1 = ybot if ybot < cb else cb
        if y0 < 0: y0 = 0
        if y1 > H - 1: y1 = H - 1
        if y1 < y0: return
        base = (y0 * W + x) * 3
        if tex is None:                              # no texture -> flat colour
            r = int(fb[0]*f); g = int(fb[1]*f); b = int(fb[2]*f)
            for _ in range(y0, y1 + 1):
                buf[base] = r; buf[base+1] = g; buf[base+2] = b; base += W*3
            return
        tw, th, grid = tex
        col = (int(tc) % tw) * th
        v = v0 + (y0 - ytop) * dv
        for _ in range(y0, y1 + 1):
            p = pal[grid[col + (int(v) % th)]]
            buf[base] = int(p[0]*f); buf[base+1] = int(p[1]*f); buf[base+2] = int(p[2]*f)
            base += W*3; v += dv

    def draw_seg(seg):
        v1, v2, ang, ldi, side, off = seg
        fs, bs = seg_sectors(m, seg)
        if fs is None: return
        ld = m.lines[ldi]
        fsd = m.sides[ld[6] if side else ld[5]]      # front sidedef
        xoff = fsd[0]; yoff = fsd[1]
        mid_t = tx.get(texname(fsd[4])) if texname(fsd[4]) else None
        up_t  = tx.get(texname(fsd[2])) if texname(fsd[2]) else None
        lo_t  = tx.get(texname(fsd[3])) if texname(fsd[3]) else None
        ax, ay = m.verts[v1]; bx, by = m.verts[v2]
        wk = 1.12 if abs(bx - ax) > abs(by - ay) else 0.88    # fake contrast by facing
        d1x, d1y = ax - px, ay - py
        d2x, d2y = bx - px, by - py
        depth1 = d1x * cos_a + d1y * sin_a
        depth2 = d2x * cos_a + d2y * sin_a
        sx1 = d1y * cos_a - d1x * sin_a
        sx2 = d2y * cos_a - d2x * sin_a
        wlen = math.hypot(bx - ax, by - ay)
        u1 = off + xoff; u2 = u1 + wlen              # texture u at v1, v2
        if depth1 < NEAR and depth2 < NEAR: return
        if depth1 < NEAR:
            t = (NEAR - depth1) / (depth2 - depth1)
            sx1 += t * (sx2 - sx1); u1 += t * (u2 - u1); depth1 = NEAR
        elif depth2 < NEAR:
            t = (NEAR - depth2) / (depth1 - depth2)
            sx2 += t * (sx1 - sx2); u2 += t * (u1 - u2); depth2 = NEAR
        rx1 = (W / 2) + (sx1 / depth1) * scale
        rx2 = (W / 2) + (sx2 / depth2) * scale
        if rx1 >= rx2: return                     # back-face / degenerate
        ix1 = max(0, int(math.ceil(rx1))); ix2 = min(W - 1, int(math.floor(rx2)))
        if ix1 > ix2: return
        inv1, inv2 = 1.0 / depth1, 1.0 / depth2
        uz1 = u1 * inv1; uz2 = u2 * inv2
        fc, ff, flight = fs[1], fs[0], fs[4]
        if bs is not None:
            bc, bf = bs[1], bs[0]
        lf = 0.72 + 0.28 * (flight / 255.0)
        span = rx2 - rx1
        for x in range(ix1, ix2 + 1):
            if ceilc[x] > floorc[x]:
                continue
            t = (x - rx1) / span
            invd = inv1 + (inv2 - inv1) * t
            dist = 1.0 / invd
            u = (uz1 + (uz2 - uz1) * t) / invd       # perspective-correct texel u
            df = 1200.0 / (dist + 650.0)
            if df > 1.0: df = 1.0
            elif df < 0.68: df = 0.68
            f = lf * df; fw = f * wk
            dv = dist / vscale                        # texels per screen row
            yfc = int(hh - (fc - viewz) * vscale * invd)
            yff = int(hh - (ff - viewz) * vscale * invd)
            ct, cb = ceilc[x], floorc[x]
            if yfc > ct:                              # ceiling
                vspan(x, ct, min(yfc - 1, cb), shade(CEIL, dist, flight))
            if yff < cb:                              # floor
                vspan(x, max(yff + 1, ct), cb, shade(FLOORC, dist, flight))
            if bs is None:                            # solid wall
                wall_col(x, yfc, yff, ct, cb, mid_t, u, yoff, dv, fw, WALL)
                ceilc[x] = 1; floorc[x] = 0
            else:
                ybc = int(hh - (bc - viewz) * vscale * invd)
                ybf = int(hh - (bf - viewz) * vscale * invd)
                newct = ct if ct > yfc else yfc
                newcb = cb if cb < yff else yff
                if bc < fc:                           # upper step (pegged to fc)
                    fbc = UPPER if up_t else CEIL     # no texture -> blend w/ ceiling
                    wall_col(x, yfc, ybc - 1, ct, cb, up_t, u, yoff, dv, fw if up_t else f, fbc)
                    if ybc > newct: newct = ybc
                if bf > ff:                           # lower step (pegged to bf)
                    fbc = LOWER if lo_t else FLOORC   # no texture -> blend w/ floor
                    wall_col(x, ybf + 1, yff, ct, cb, lo_t, u, yoff, dv, fw if lo_t else f, fbc)
                    if ybf < newcb: newcb = ybf
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
        tx = Textures(wad)
        px, py, pa = m.player_start()
        deg = float(sys.argv[2]) if len(sys.argv) > 2 else pa
        render_view(m, px, py, math.radians(deg), os.path.join(out, "duum_view.ppm"), tx)
