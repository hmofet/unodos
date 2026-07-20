# DUUM.PY - a Doom engine for UnoDOS, written in Python.
#
# The acceptance test that Python is a first-class app language here: a subset
# Doom renderer that loads a real IWAD (DOOM1.WAD / freedoom1.wad) and draws a
# first-person, BSP-traversed, TEXTURE-MAPPED view of E1M1, walkable with the
# arrow keys.
#
# It exercises the whole platform from Python: file I/O (uno.read_at streams the
# WAD - directory, map lumps, palette, textures and patches - never slurped),
# heavy compute (BSP walk + column renderer + texture composition), the
# framebuffer (uno canvas, incl. the cv.wall_col fast path), input and float
# math.  Mirrors tools/duum_host.py, proven on the host first.
#
# Controls: Up/Down move, Left/Right turn, comma/period strafe.

import uno
import struct
import math

WADNAME = "DOOM1.WAD"
LEVEL = "E1M1"

FOV = math.pi / 2
NEAR = 4.0
EYE = 41
VER = 1.5
MOVE = 12.0
TURN = 0.20


def rgb(r, g, b):
    if r > 255: r = 255
    if g > 255: g = 255
    if b > 255: b = 255
    return 0xFF000000 | (b << 16) | (g << 8) | r


# ---- streaming WAD reader --------------------------------------------------
class Wad:
    def __init__(self, name):
        self.name = name
        self.vol = self._find_vol(name)
        hdr = uno.read_at(self.vol, name, 0, 12)
        magic, self.n, self.diro = struct.unpack_from("<4sII", hdr, 0)
        self.dir = []
        self.byname = {}
        d = uno.read_at(self.vol, name, self.diro, self.n * 16)
        for i in range(self.n):
            off, sz, nm = struct.unpack_from("<II8s", d, 16 * i)
            j = nm.find(b"\0")
            if j >= 0: nm = nm[:j]
            self.dir.append((nm, off, sz))
            if nm not in self.byname:
                self.byname[nm] = (off, sz)

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

    def lump(self, name):
        b = name.encode() if isinstance(name, str) else name
        e = self.byname.get(b)
        if e is None or e[1] == 0:
            return b""
        return uno.read_at(self.vol, self.name, e[0], e[1])

    def lump_at(self, i):
        _, off, sz = self.dir[i]
        return uno.read_at(self.vol, self.name, off, sz) if sz else b""


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
        want = (b"VERTEXES", b"LINEDEFS", b"SIDEDEFS", b"SEGS",
                b"SSECTORS", b"NODES", b"SECTORS", b"THINGS")
        lp = {}
        for j in range(mi + 1, mi + 12):
            nm = wad.dir[j][0]
            if nm in want:
                lp[nm] = wad.lump_at(j)
        self.verts    = rows(lp[b"VERTEXES"], "<hh", 4)
        self.lines    = rows(lp[b"LINEDEFS"], "<HHHHHHH", 14)
        self.sides    = rows(lp[b"SIDEDEFS"], "<hh8s8s8sH", 30)
        self.segs     = rows(lp[b"SEGS"], "<HHhHHh", 12)
        self.ssectors = rows(lp[b"SSECTORS"], "<HH", 4)
        self.nodes    = rows(lp[b"NODES"], "<hhhh8hHH", 28)
        self.sectors  = rows(lp[b"SECTORS"], "<hh8s8shHH", 26)
        self.things   = rows(lp[b"THINGS"], "<hhHHH", 10)

    def player_start(self):
        for x, y, ang, typ, fl in self.things:
            if typ == 1:
                return x, y, ang
        return 0, 0, 0


# ---- textures --------------------------------------------------------------
def _texname(b):
    j = b.find(b"\0")
    s = (b[:j] if j >= 0 else b).decode("latin1")
    return s if (s and s != "-") else None


class Textures:
    def __init__(self, wad):
        self.wad = wad
        d = wad.lump("PLAYPAL")
        self.pal = bytes(d[:768]) if len(d) >= 768 else (bytes(768))
        # PNAMES
        pn = wad.lump("PNAMES")
        cnt = struct.unpack_from("<I", pn, 0)[0]
        self.pnames = []
        for i in range(cnt):
            nm = pn[4 + i*8:4 + i*8 + 8]
            j = nm.find(b"\0")
            self.pnames.append((nm[:j] if j >= 0 else nm))
        # TEXTURE1/2 definitions
        self.defs = {}
        for tl in ("TEXTURE1", "TEXTURE2"):
            self._parse_tex(wad.lump(tl))
        self.cache = {}

    def _parse_tex(self, d):
        if not d:
            return
        count = struct.unpack_from("<I", d, 0)[0]
        offs = struct.unpack_from("<%dI" % count, d, 4)
        for o in offs:
            nm = d[o:o+8]
            j = nm.find(b"\0")
            name = (nm[:j] if j >= 0 else nm).decode("latin1").upper()
            w, h = struct.unpack_from("<HH", d, o+12)
            pc = struct.unpack_from("<H", d, o+20)[0]
            pats = []
            for p in range(pc):
                ox, oy, pidx = struct.unpack_from("<hhH", d, o+22+p*10)
                if pidx < len(self.pnames):
                    pats.append((ox, oy, self.pnames[pidx]))
            self.defs[name] = (w, h, pats)

    def _decode_patch(self, name):
        d = self.wad.lump(name)
        if not d:
            return None
        w, h, lo, to = struct.unpack_from("<HHhh", d, 0)
        colofs = struct.unpack_from("<%dI" % w, d, 8)
        cols = []
        for c in range(w):
            posts = []
            p = colofs[c]
            n = len(d)
            while p < n and d[p] != 0xFF:
                top = d[p]; length = d[p+1]
                posts.append((top, d[p+3:p+3+length]))
                p += length + 4
            cols.append(posts)
        return w, h, cols

    def get(self, name):
        if name is None:
            return None
        name = name.upper()
        t = self.cache.get(name)
        if t is not None or name in self.cache:
            return t
        td = self.defs.get(name)
        if td is None:
            self.cache[name] = None
            return None
        w, h, pats = td
        grid = bytearray(w * h)          # column-major: grid[x*h + y]
        for (ox, oy, pn) in pats:
            pat = self._decode_patch(pn)
            if pat is None:
                continue
            pw, ph, cols = pat
            for cx in range(pw):
                dx = ox + cx
                if dx < 0 or dx >= w:
                    continue
                base = dx * h
                for (top, pix) in cols[cx]:
                    dy0 = oy + top
                    for i in range(len(pix)):
                        dy = dy0 + i
                        if 0 <= dy < h:
                            grid[base + dy] = pix[i]
        t = (w, h, grid)
        self.cache[name] = t
        return t


# ---- the engine ------------------------------------------------------------
class Duum(uno.App):

    def build(self, cv):
        self.err = None
        self.frame = []
        self.cw = cv.width()
        self.ch = cv.height()
        try:
            self.wad = Wad(WADNAME)
            self.lvl = Level(self.wad, LEVEL)
            self.tex = Textures(self.wad)
            self.pal = self.tex.pal
            px, py, pa = self.lvl.player_start()
            self.px = float(px); self.py = float(py)
            self.pa = math.radians(pa)
            self.dirty = True
        except Exception as e:
            self.err = repr(e)

    def seg_sectors(self, seg):
        m = self.lvl
        ld = m.lines[seg[3]]
        side = seg[4]
        fsd = ld[6] if side else ld[5]
        bsd = ld[5] if side else ld[6]
        fs = m.sectors[m.sides[fsd][5]] if fsd != 0xFFFF else None
        bs = m.sectors[m.sides[bsd][5]] if bsd != 0xFFFF else None
        return fs, bs, m.sides[fsd]

    def point_sector(self, px, py):
        m = self.lvl
        n = len(m.nodes) - 1
        while not (n & 0x8000):
            nd = m.nodes[n]
            side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
            n = nd[-2] if side <= 0 else nd[-1]
        fs, _, _ = self.seg_sectors(m.segs[m.ssectors[n & 0x7FFF][1]])
        return fs

    def render(self):
        m = self.lvl; tex = self.tex
        cw = self.cw; ch = self.ch
        RW = cw if cw < 220 else 220        # internal columns (caps Python work)
        colw = cw // RW
        if colw < 1: colw = 1
        px, py, pa = self.px, self.py, self.pa
        cos_a = math.cos(pa); sin_a = math.sin(pa)
        ps = self.point_sector(px, py)
        viewz = (ps[0] if ps else 0) + EYE
        scale = (RW / 2) / math.tan(FOV / 2)         # column-space (for rx)
        vsc = ((cw / 2) / math.tan(FOV / 2)) * VER   # canvas-px vertical scale
        hh = ch / 2
        ceilc = [0] * RW
        floorc = [ch - 1] * RW
        out = []
        verts = m.verts
        segs = m.segs
        C_CEIL = (84, 88, 104); C_FLOOR = (120, 100, 74); C_SKY = (96, 120, 170)

        def col_rgb(base, f):
            return rgb(int(base[0]*f), int(base[1]*f), int(base[2]*f))

        def rect(cx, y0, y1, color):
            if y1 < y0: return
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            x0 = cx * cw // RW; x1 = (cx + 1) * cw // RW      # exact tiling (no gap)
            out.append(('R', x0, y0, x1 - x0, y1 - y0 + 1, color))

        def wall(cx, ytop, ybot, ct, cb, tx, u, v_top, dv, sh, fb):
            y0 = ytop if ytop > ct else ct
            y1 = ybot if ybot < cb else cb
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            if y1 < y0: return
            x0 = cx * cw // RW; x1 = (cx + 1) * cw // RW
            if tx is None:
                out.append(('R', x0, y0, x1 - x0, y1 - y0 + 1,
                            rgb((fb[0]*sh) >> 8, (fb[1]*sh) >> 8, (fb[2]*sh) >> 8)))
                return
            tw, th, grid = tx
            v0 = int((v_top + (y0 - ytop) * dv) * 256)
            out.append(('W', x0, x1 - x0, y0, y1 - y0 + 1,
                        grid, tw, th, int(u), v0, int(dv * 256), sh))

        def draw_seg(seg):
            fs, bs, fsd = self.seg_sectors(seg)
            if fs is None: return
            ax, ay = verts[seg[0]]; bx, by = verts[seg[1]]
            wk = 115 if abs(bx - ax) > abs(by - ay) else 88     # /100 contrast
            d1x = ax - px; d1y = ay - py
            d2x = bx - px; d2y = by - py
            depth1 = d1x * cos_a + d1y * sin_a
            depth2 = d2x * cos_a + d2y * sin_a
            sx1 = d1y * cos_a - d1x * sin_a
            sx2 = d2y * cos_a - d2x * sin_a
            wlen = math.sqrt((bx-ax)*(bx-ax) + (by-ay)*(by-ay))
            u1 = seg[5] + fsd[0]; u2 = u1 + wlen
            if depth1 < NEAR and depth2 < NEAR: return
            if depth1 < NEAR:
                t = (NEAR - depth1) / (depth2 - depth1)
                sx1 += t * (sx2 - sx1); u1 += t * (u2 - u1); depth1 = NEAR
            elif depth2 < NEAR:
                t = (NEAR - depth2) / (depth1 - depth2)
                sx2 += t * (sx1 - sx2); u2 += t * (u1 - u2); depth2 = NEAR
            rx1 = (RW / 2) + (sx1 / depth1) * scale
            rx2 = (RW / 2) + (sx2 / depth2) * scale
            if rx1 >= rx2: return
            ix1 = int(rx1 + 0.999); ix2 = int(rx2)
            if ix1 < 0: ix1 = 0
            if ix2 > RW - 1: ix2 = RW - 1
            if ix1 > ix2: return
            inv1 = 1.0 / depth1; inv2 = 1.0 / depth2
            uz1 = u1 * inv1; uz2 = u2 * inv2
            fc = fs[1]; ff = fs[0]; flight = fs[4]
            yoff = fsd[1]
            mid_t = self.tex.get(_texname(fsd[4]))
            up_t = self.tex.get(_texname(fsd[2]))
            lo_t = self.tex.get(_texname(fsd[3]))
            twosided = bs is not None
            if twosided:
                bc = bs[1]; bf = bs[0]
            lf = 0.72 + 0.28 * (flight / 255.0)
            span = rx2 - rx1
            for x in range(ix1, ix2 + 1):
                ct = ceilc[x]; cb = floorc[x]
                if ct > cb:
                    continue
                t = (x - rx1) / span
                invd = inv1 + (inv2 - inv1) * t
                dist = 1.0 / invd
                u = (uz1 + (uz2 - uz1) * t) / invd
                df = 1200.0 / (dist + 650.0)
                if df > 1.0: df = 1.0
                elif df < 0.68: df = 0.68
                sh = int(lf * df * 256)
                shw = (sh * wk) // 100
                dv = dist / vsc
                yfc = int(hh - (fc - viewz) * vsc * invd)
                yff = int(hh - (ff - viewz) * vsc * invd)
                if yfc > ct:
                    rect(x, ct, (yfc - 1) if (yfc - 1) < cb else cb, col_rgb(C_CEIL, lf*0.9))
                if yff < cb:
                    rect(x, (yff + 1) if (yff + 1) > ct else ct, cb, col_rgb(C_FLOOR, lf*0.9))
                if not twosided:
                    wall(x, yfc, yff, ct, cb, mid_t, u, yoff, dv, shw, (200, 188, 170))
                    ceilc[x] = 1; floorc[x] = 0
                else:
                    ybc = int(hh - (bc - viewz) * vsc * invd)
                    ybf = int(hh - (bf - viewz) * vsc * invd)
                    newct = ct if ct > yfc else yfc
                    newcb = cb if cb < yff else yff
                    if bc < fc:
                        if up_t:
                            wall(x, yfc, ybc - 1, ct, cb, up_t, u, yoff, dv, shw, (172, 150, 122))
                        else:
                            rect(x, yfc if yfc > ct else ct, (ybc-1) if (ybc-1) < cb else cb, col_rgb(C_CEIL, lf*df))
                        if ybc > newct: newct = ybc
                    if bf > ff:
                        if lo_t:
                            wall(x, ybf + 1, yff, ct, cb, lo_t, u, yoff, dv, shw, (140, 150, 170))
                        else:
                            rect(x, (ybf+1) if (ybf+1) > ct else ct, yff if yff < cb else cb, col_rgb(C_FLOOR, lf*df))
                        if ybf < newcb: newcb = ybf
                    ceilc[x] = newct; floorc[x] = newcb

        def walk(n):
            while True:
                if n & 0x8000:
                    ss = m.ssectors[n & 0x7FFF]
                    first = ss[1]
                    for i in range(first, first + ss[0]):
                        draw_seg(segs[i])
                    return
                nd = m.nodes[n]
                side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
                if side <= 0:
                    walk(nd[-2]); n = nd[-1]
                else:
                    walk(nd[-1]); n = nd[-2]

        walk(len(m.nodes) - 1)
        sky = rgb(*C_SKY)
        for x in range(RW):
            if ceilc[x] <= floorc[x]:
                x0 = x * cw // RW; x1 = (x + 1) * cw // RW
                out.append(('R', x0, ceilc[x], x1 - x0, floorc[x] - ceilc[x] + 1, sky))
        self.frame = out

    def draw(self, cv):
        if self.err is not None:
            cv.clear(rgb(20, 20, 28))
            cv.text(8, 8, "Duum: could not load " + WADNAME, rgb(255, 120, 120))
            cv.text(8, 26, self.err[:44], rgb(200, 200, 200))
            cv.text(8, 52, "Put a WAD on the disk (see wads/README).", rgb(180, 180, 180))
            return
        cv.clear(rgb(0, 0, 0))
        pal = self.pal
        for op in self.frame:
            if op[0] == 'R':
                cv.fill_rect(op[1], op[2], op[3], op[4], op[5])
            else:
                _, x, wpx, y0, count, grid, tw, th, tc, v0, dv, sh = op
                xx = x
                for _2 in range(wpx):
                    cv.wall_col(xx, y0, count, grid, tw, th, tc, v0, dv, pal, sh)
                    xx += 1
        cv.text(6, 6, LEVEL + "  arrows move/turn", rgb(235, 235, 210))

    def tick(self):
        if self.err is None and self.dirty:
            self.render()
            self.dirty = False

    def _blocked(self, nx, ny):
        s = self.point_sector(nx, ny)
        if s is None:
            return True
        cur = self.point_sector(self.px, self.py)
        if cur is not None and s[0] - cur[0] > 32:
            return True
        if s[1] - s[0] < 56:
            return True
        return False

    def key(self, uni, scan, ctrl):
        if self.err is not None:
            return False
        ca = math.cos(self.pa); sa = math.sin(self.pa)
        nx, ny = self.px, self.py
        # UnoDOS arrow scancodes: Up=1 Down=2 Right=3 Left=4
        if scan == 0x04:
            self.pa -= TURN; self.dirty = True; return True
        if scan == 0x03:
            self.pa += TURN; self.dirty = True; return True
        if scan == 0x01:
            nx = self.px + ca * MOVE; ny = self.py + sa * MOVE
        elif scan == 0x02:
            nx = self.px - ca * MOVE; ny = self.py - sa * MOVE
        elif uni == ord(','):
            nx = self.px + sa * MOVE; ny = self.py - ca * MOVE
        elif uni == ord('.'):
            nx = self.px - sa * MOVE; ny = self.py + ca * MOVE
        else:
            return False
        if not self._blocked(nx, ny):
            self.px = nx; self.py = ny
        self.dirty = True
        return True


app = Duum()
