# DUUM.PY - a Doom engine for UnoDOS, written in Python.
#
# The acceptance test that Python is a first-class app language here: a subset
# Doom renderer that loads a real IWAD (DOOM1.WAD / freedoom1.wad) and draws a
# first-person, BSP-traversed view of E1M1, walkable with the arrow keys.
#
# It exercises the whole platform from Python: file I/O (uno.read_at streams the
# ~4-28 MB WAD a lump at a time, never slurped), heavy compute (BSP walk + the
# column renderer), the framebuffer (uno canvas), input (key callbacks) and
# float math (projection/trig).  The engine mirrors tools/duum_host.py, proven
# on the host first.
#
# Controls: Up/Down move, Left/Right turn, comma/period strafe.

import uno
import struct
import math

WADNAME = "DOOM1.WAD"      # staged on the ESP root by build.sh (Freedoom default)
LEVEL = "E1M1"

FOV = math.pi / 2
NEAR = 4.0
EYE = 41
MOVE = 12.0
TURN = 0.20


def rgb(r, g, b):
    if r > 255: r = 255
    if g > 255: g = 255
    if b > 255: b = 255
    return 0xFF000000 | (b << 16) | (g << 8) | r     # matches uno.rgb packing


# ---- streaming WAD reader --------------------------------------------------
class Wad:
    def __init__(self, name):
        self.name = name
        self.vol = self._find_vol(name)
        hdr = uno.read_at(self.vol, name, 0, 12)
        magic, self.n, self.diro = struct.unpack_from("<4sII", hdr, 0)
        # the directory (small enough to hold): 16 bytes per lump
        self.dir = []
        d = uno.read_at(self.vol, name, self.diro, self.n * 16)
        for i in range(self.n):
            off, sz, nm = struct.unpack_from("<II8s", d, 16 * i)
            j = nm.find(b"\0")
            if j >= 0: nm = nm[:j]
            self.dir.append((nm, off, sz))

    def _find_vol(self, name):
        for v in range(8):
            if uno.size(v, name) > 0:
                return v
        return 0

    def index(self, name):
        b = name.encode() if isinstance(name, str) else name
        for i in range(self.n):
            if self.dir[i][0] == b:
                return i
        return -1

    def lump(self, i):
        _, off, sz = self.dir[i]
        if sz == 0:
            return b""
        return uno.read_at(self.vol, self.name, off, sz)


def rows(data, fmt, sz):
    out = []
    n = len(data)
    k = 0
    while k + sz <= n:
        out.append(struct.unpack_from(fmt, data, k))
        k += sz
    return out


class Level:
    def __init__(self, wad, name):
        mi = wad.index(name)
        want = {b"VERTEXES", b"LINEDEFS", b"SIDEDEFS", b"SEGS",
                b"SSECTORS", b"NODES", b"SECTORS", b"THINGS"}
        lumps = {}
        for j in range(mi + 1, mi + 12):
            nm = wad.dir[j][0]
            if nm in want:
                lumps[nm] = wad.lump(j)
        self.verts    = rows(lumps[b"VERTEXES"], "<hh", 4)
        self.lines    = rows(lumps[b"LINEDEFS"], "<HHHHHHH", 14)
        self.sides    = rows(lumps[b"SIDEDEFS"], "<hh8s8s8sH", 30)
        self.segs     = rows(lumps[b"SEGS"], "<HHhHHh", 12)
        self.ssectors = rows(lumps[b"SSECTORS"], "<HH", 4)
        self.nodes    = rows(lumps[b"NODES"], "<hhhh8hHH", 28)
        self.sectors  = rows(lumps[b"SECTORS"], "<hh8s8shHH", 26)
        self.things   = rows(lumps[b"THINGS"], "<hhHHH", 10)

    def player_start(self):
        for x, y, ang, typ, fl in self.things:
            if typ == 1:
                return x, y, ang
        return 0, 0, 0


# ---- the engine ------------------------------------------------------------
class Duum(uno.App):

    def build(self, cv):
        self.err = None
        self.frame = []            # cached render: list of (x, y0, y1, colour)
        self.cw = cv.width()
        self.ch = cv.height()
        try:
            self.wad = Wad(WADNAME)
            self.lvl = Level(self.wad, LEVEL)
            px, py, pa = self.lvl.player_start()
            self.px = float(px); self.py = float(py)
            self.pa = math.radians(pa)
            self.dirty = True
        except Exception as e:
            self.err = repr(e)

    # ---- BSP helpers -------------------------------------------------------
    def seg_sectors(self, seg):
        m = self.lvl
        v1, v2, ang, ldi, side, off = seg
        ld = m.lines[ldi]
        fsd = ld[6] if side else ld[5]
        bsd = ld[5] if side else ld[6]
        fs = m.sectors[m.sides[fsd][5]] if fsd != 0xFFFF else None
        bs = m.sectors[m.sides[bsd][5]] if bsd != 0xFFFF else None
        return fs, bs

    def point_sector(self, px, py):
        m = self.lvl
        n = len(m.nodes) - 1
        while not (n & 0x8000):
            nd = m.nodes[n]
            side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
            n = nd[-2] if side <= 0 else nd[-1]
        ss = m.ssectors[n & 0x7FFF]
        fs, _ = self.seg_sectors(m.segs[ss[1]])
        return fs

    # ---- render one frame into self.frame ---------------------------------
    def render(self):
        m = self.lvl
        RW = self.cw if self.cw < 200 else 200        # internal columns (cap work)
        RH = self.ch if self.ch < 140 else 140
        rowh = self.ch / RH
        px, py, pa = self.px, self.py, self.pa
        cos_a = math.cos(pa); sin_a = math.sin(pa)
        ps = self.point_sector(px, py)
        viewz = (ps[0] if ps else 0) + EYE
        scale = (RW / 2) / math.tan(FOV / 2)
        hh = RH / 2
        ceilc = [0] * RW
        floorc = [RH - 1] * RW
        out = []
        verts = m.verts

        # surface colours (packed once)
        C_CEIL = rgb(84, 88, 104); C_FLOOR = rgb(120, 100, 74)
        C_SKY = rgb(96, 120, 170)

        def shade(r, g, b, dist, light, k):
            f = (0.72 + 0.28 * (light / 255.0)) * k
            df = 1100.0 / (dist + 700.0)
            if df > 1.0: df = 1.0
            elif df < 0.6: df = 0.6
            f *= df
            return rgb(int(r * f), int(g * f), int(b * f))

        segs = m.segs
        sensectors = self.seg_sectors

        def draw_seg(seg):
            fs, bs = sensectors(seg)
            if fs is None:
                return
            ax, ay = verts[seg[0]]; bx, by = verts[seg[1]]
            wk = 1.15 if abs(bx - ax) > abs(by - ay) else 0.85
            d1x = ax - px; d1y = ay - py
            d2x = bx - px; d2y = by - py
            depth1 = d1x * cos_a + d1y * sin_a
            depth2 = d2x * cos_a + d2y * sin_a
            sx1 = d1y * cos_a - d1x * sin_a
            sx2 = d2y * cos_a - d2x * sin_a
            if depth1 < NEAR and depth2 < NEAR:
                return
            if depth1 < NEAR:
                t = (NEAR - depth1) / (depth2 - depth1)
                sx1 += t * (sx2 - sx1); depth1 = NEAR
            elif depth2 < NEAR:
                t = (NEAR - depth2) / (depth1 - depth2)
                sx2 += t * (sx1 - sx2); depth2 = NEAR
            rx1 = (RW / 2) + (sx1 / depth1) * scale
            rx2 = (RW / 2) + (sx2 / depth2) * scale
            if rx1 >= rx2:
                return
            ix1 = int(rx1 + 0.999)
            ix2 = int(rx2)
            if ix1 < 0: ix1 = 0
            if ix2 > RW - 1: ix2 = RW - 1
            if ix1 > ix2:
                return
            inv1 = 1.0 / depth1; inv2 = 1.0 / depth2
            fc = fs[1]; ff = fs[0]; flight = fs[4]
            twosided = bs is not None
            if twosided:
                bc = bs[1]; bf = bs[0]
            span = rx2 - rx1
            for x in range(ix1, ix2 + 1):
                ct = ceilc[x]; cb = floorc[x]
                if ct > cb:
                    continue
                t = (x - rx1) / span
                invd = inv1 + (inv2 - inv1) * t
                dist = 1.0 / invd
                sd = scale * invd
                yfc = int(hh - (fc - viewz) * sd)
                yff = int(hh - (ff - viewz) * sd)
                if yfc > ct:
                    y1 = yfc - 1
                    if y1 > cb: y1 = cb
                    out.append((x, ct, y1, C_CEIL))
                if yff < cb:
                    y0 = yff + 1
                    if y0 < ct: y0 = ct
                    out.append((x, y0, cb, C_FLOOR))
                if not twosided:
                    y0 = yfc if yfc > ct else ct
                    y1 = yff if yff < cb else cb
                    out.append((x, y0, y1, shade(200, 188, 170, dist, flight, wk)))
                    ceilc[x] = 1; floorc[x] = 0
                else:
                    ybc = int(hh - (bc - viewz) * sd)
                    ybf = int(hh - (bf - viewz) * sd)
                    newct = ct if ct > yfc else yfc
                    newcb = cb if cb < yff else yff
                    if bc < fc:
                        y0 = yfc if yfc > ct else ct
                        y1 = ybc - 1
                        if y1 > cb: y1 = cb
                        if y1 >= y0:
                            out.append((x, y0, y1, shade(172, 150, 122, dist, flight, wk)))
                        if ybc > newct: newct = ybc
                    if bf > ff:
                        y0 = ybf + 1
                        if y0 < ct: y0 = ct
                        y1 = yff if yff < cb else cb
                        if y1 >= y0:
                            out.append((x, y0, y1, shade(140, 150, 170, dist, flight, wk)))
                        if ybf < newcb: newcb = ybf
                    ceilc[x] = newct; floorc[x] = newcb

        def walk(n):
            while True:
                if n & 0x8000:
                    ss = m.ssectors[n & 0x7FFF]
                    cnt = ss[0]; first = ss[1]
                    for i in range(first, first + cnt):
                        draw_seg(segs[i])
                    return
                nd = m.nodes[n]
                side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
                if side <= 0:
                    walk(nd[-2]); n = nd[-1]
                else:
                    walk(nd[-1]); n = nd[-2]

        walk(len(m.nodes) - 1)
        # sky fill for still-open columns
        for x in range(RW):
            if ceilc[x] <= floorc[x]:
                out.append((x, ceilc[x], floorc[x], C_SKY))

        # scale internal columns/rows up to the canvas and cache as rects
        frame = []
        cw = self.cw
        for (x, y0, y1, col) in out:
            cx0 = x * cw // RW
            cx1 = (x + 1) * cw // RW
            cy0 = int(y0 * rowh)
            cy1 = int((y1 + 1) * rowh)
            frame.append((cx0, cy0, cx1 - cx0, cy1 - cy0, col))
        self.frame = frame

    # ---- app callbacks -----------------------------------------------------
    def draw(self, cv):
        if self.err is not None:
            cv.clear(rgb(20, 20, 28))
            cv.text(8, 8, "Duum: could not load " + WADNAME, rgb(255, 120, 120))
            cv.text(8, 26, self.err[:40], rgb(200, 200, 200))
            cv.text(8, 52, "Put DOOM1.WAD on the disk (see wads/README).", rgb(180, 180, 180))
            return
        cv.clear(rgb(0, 0, 0))
        for (x, y, w, h, col) in self.frame:
            cv.fill_rect(x, y, w, h, col)
        cv.text(6, 6, LEVEL + "  arrows move/turn", rgb(230, 230, 210))

    def tick(self):
        if self.err is None and self.dirty:
            self.render()
            self.dirty = False

    def _blocked(self, nx, ny):
        # crude collision: reject if the destination sector floor steps up too
        # far from the current one (walls bound sectors; a big step ~ a wall).
        s = self.point_sector(nx, ny)
        if s is None:
            return True
        cur = self.point_sector(self.px, self.py)
        if cur is not None and s[0] - cur[0] > 32:
            return True
        if s[1] - s[0] < 56:            # too low a ceiling to enter
            return True
        return False

    def key(self, uni, scan, ctrl):
        if self.err is not None:
            return False
        m = MOVE
        ca = math.cos(self.pa); sa = math.sin(self.pa)
        nx, ny = self.px, self.py
        # arrow scancodes (UnoDOS): Up=0x01 Down=0x02 Right=0x03 Left=0x04
        if scan == 0x04:
            self.pa -= TURN; self.dirty = True; return True
        if scan == 0x03:
            self.pa += TURN; self.dirty = True; return True
        if scan == 0x01:
            nx = self.px + ca * m; ny = self.py + sa * m
        elif scan == 0x02:
            nx = self.px - ca * m; ny = self.py - sa * m
        elif uni == ord(','):
            nx = self.px + sa * m; ny = self.py - ca * m
        elif uni == ord('.'):
            nx = self.px - sa * m; ny = self.py + ca * m
        else:
            return False
        if not self._blocked(nx, ny):
            self.px = nx; self.py = ny
        self.dirty = True
        return True


app = Duum()
