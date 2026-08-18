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
VER = 1.2          # Doom's 320x200-stretched-to-4:3 pixel aspect
MOVE = 12.0
TURN = 0.20

# linedef flags
ML_TWOSIDED   = 4
ML_DONTPEGTOP = 8
ML_DONTPEGBOT = 16


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
        # sectors are LISTS: doors/lifts mutate floor/ceiling heights live
        self.sectors  = [list(t) for t in rows(lp[b"SECTORS"], "<hh8s8shHH", 26)]
        self.things   = rows(lp[b"THINGS"], "<hhHHH", 10)
        self.skyc     = [s[3][:6] == b"F_SKY1" for s in self.sectors]

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
        self.fcache = {}

    def flat(self, name8):
        """64x64 flat lump by raw 8-byte sector field -> 4096 bytes or None."""
        f = self.fcache.get(name8)
        if f is not None or name8 in self.fcache:
            return f
        j = name8.find(b"\0")
        d = self.wad.lump(name8[:j] if j >= 0 else name8)
        f = d[:4096] if len(d) >= 4096 else None
        self.fcache[name8] = f
        return f

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


# ---- sprites ---------------------------------------------------------------
class Sprites:
    """S_START..S_END lump index + patch decoder.  Grids are column-major like
    wall textures, with 0xFF as the transparent sentinel (real palette index
    255 is remapped to 254; no stock sprite uses it visibly)."""
    def __init__(self, wad):
        self.wad = wad
        self.idx = {}
        self.cache = {}
        on = False
        for i in range(wad.n):
            nm = wad.dir[i][0]
            if nm == b"S_START" or nm == b"SS_START":
                on = True; continue
            if nm == b"S_END" or nm == b"SS_END":
                break
            if not on or len(nm) < 6:
                continue
            base = nm[:4]
            self.idx[(base, nm[4:5], nm[5] - 48)] = (i, 0)
            if len(nm) >= 8:
                self.idx[(base, nm[6:7], nm[7] - 48)] = (i, 1)

    def get(self, base, frame, rot):
        """-> (w, h, leftoffs, topoffs, grid, mirror) or None."""
        e = self.idx.get((base, frame, rot + 1))
        if e is None:
            e = self.idx.get((base, frame, 0))
        if e is None:
            return None
        li, mirror = e
        s = self.cache.get(li)
        if s is None:
            d = self.wad.lump_at(li)
            w, h, lo, to = struct.unpack_from("<HHhh", d, 0)
            colofs = struct.unpack_from("<%dI" % w, d, 8)
            grid = bytearray(b"\xff" * (w * h))
            n = len(d)
            for c in range(w):
                p = colofs[c]
                base_i = c * h
                while p < n and d[p] != 0xFF:
                    top = d[p]; ln = d[p + 1]
                    for i in range(ln):
                        v = d[p + 3 + i]
                        grid[base_i + top + i] = 254 if v == 255 else v
                    p += ln + 4
            s = (w, h, lo, to, grid)
            self.cache[li] = s
        return (s[0], s[1], s[2], s[3], s[4], mirror)


# Thing render table: type -> (sprite base, idle frame cycle, hang-from-ceiling)
# Gameplay state machines override the frame; this is what stands in the world.
THINGTAB = {
    # monsters
    3004: (b"POSS", b"AB", 0), 9: (b"SPOS", b"AB", 0), 3001: (b"TROO", b"AB", 0),
    3002: (b"SARG", b"AB", 0), 3005: (b"HEAD", b"A", 0), 3006: (b"SKUL", b"AB", 0),
    3003: (b"BOSS", b"AB", 0), 7: (b"SPID", b"AB", 0), 16: (b"CYBR", b"AB", 0),
    # weapons + ammo
    2001: (b"SHOT", b"A", 0), 2002: (b"MGUN", b"A", 0), 2003: (b"LAUN", b"A", 0),
    2004: (b"PLAS", b"A", 0), 2005: (b"CSAW", b"A", 0), 2006: (b"BFUG", b"A", 0),
    2007: (b"CLIP", b"A", 0), 2008: (b"SHEL", b"A", 0), 2010: (b"ROCK", b"A", 0),
    2046: (b"BROK", b"A", 0), 2047: (b"CELL", b"A", 0), 2048: (b"AMMO", b"A", 0),
    2049: (b"SBOX", b"A", 0), 8: (b"BPAK", b"A", 0),
    # health / armor / powerups
    2011: (b"STIM", b"A", 0), 2012: (b"MEDI", b"A", 0),
    2014: (b"BON1", b"ABCDCB", 0), 2015: (b"BON2", b"ABCDCB", 0),
    2018: (b"ARM1", b"AB", 0), 2019: (b"ARM2", b"AB", 0),
    2013: (b"SOUL", b"ABCDCB", 0), 2022: (b"PINV", b"ABCD", 0),
    2023: (b"PSTR", b"A", 0), 2024: (b"PINS", b"ABCD", 0),
    2025: (b"SUIT", b"A", 0), 2026: (b"PMAP", b"ABCDCB", 0),
    2045: (b"PVIS", b"AB", 0),
    # keys
    5: (b"BKEY", b"AB", 0), 13: (b"RKEY", b"AB", 0), 6: (b"YKEY", b"AB", 0),
    # decorations
    2035: (b"BAR1", b"AB", 0), 2028: (b"COLU", b"A", 0),
    30: (b"COL1", b"A", 0), 31: (b"COL2", b"A", 0), 32: (b"COL3", b"A", 0),
    33: (b"COL4", b"A", 0), 36: (b"COL5", b"AB", 0), 37: (b"COL6", b"A", 0),
    41: (b"CEYE", b"ABCB", 0), 42: (b"FSKU", b"ABC", 0),
    43: (b"TRE1", b"A", 0), 54: (b"TRE2", b"A", 0), 47: (b"SMIT", b"A", 0),
    44: (b"TBLU", b"ABCD", 0), 45: (b"TGRN", b"ABCD", 0), 46: (b"TRED", b"ABCD", 0),
    55: (b"SMBT", b"ABCD", 0), 56: (b"SMGT", b"ABCD", 0), 57: (b"SMRT", b"ABCD", 0),
    34: (b"CAND", b"A", 0), 35: (b"CBRA", b"A", 0),
    27: (b"POL4", b"A", 0), 28: (b"POL2", b"A", 0), 29: (b"POL3", b"AB", 0),
    25: (b"POL1", b"A", 0), 26: (b"POL6", b"AB", 0),
    10: (b"PLAY", b"W", 0), 12: (b"PLAY", b"W", 0), 15: (b"PLAY", b"N", 0),
    18: (b"POSS", b"L", 0), 19: (b"SPOS", b"L", 0), 20: (b"TROO", b"M", 0),
    21: (b"SARG", b"N", 0), 22: (b"HEAD", b"L", 0), 24: (b"POB2", b"A", 0),
    # hanging (ceiling)
    48: (b"ELEC", b"A", 0), 49: (b"GOR1", b"ABCB", 1), 50: (b"GOR2", b"A", 1),
    51: (b"GOR3", b"A", 1), 52: (b"GOR4", b"A", 1), 53: (b"GOR5", b"A", 1),
    59: (b"GOR2", b"A", 1), 60: (b"GOR4", b"A", 1), 61: (b"GOR3", b"A", 1),
    62: (b"GOR5", b"A", 1), 63: (b"GOR1", b"ABCB", 1),
}


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
            self.sky = self.tex.get("SKY" + LEVEL[1])   # E<n> -> SKY<n>
            self.spr = Sprites(self.wad)
            px, py, pa = self.lvl.player_start()
            self.px = float(px); self.py = float(py)
            self.pa = math.radians(pa)
            self.load_things()
            self.dirty = True
        except Exception as e:
            self.err = repr(e)

    # live thing record indices
    TX = 0; TY = 1; TANG = 2; TTYPE = 3; TSPR = 4; TFRAMES = 5; THANG = 6
    TSEC = 7; TFI = 8

    def load_things(self):
        self.things_live = []
        for (x, y, ang, typ, fl) in self.lvl.things:
            if fl & 16:                      # multiplayer-only
                continue
            info = THINGTAB.get(typ)
            if info is None:
                continue
            sec = self.point_sector(x, y)
            if sec is None:
                continue
            self.things_live.append([float(x), float(y), math.radians(ang),
                                     typ, info[0], info[1], info[2], sec, 0])

    def seg_sectors(self, seg):
        m = self.lvl
        ld = m.lines[seg[3]]
        side = seg[4]
        fsd = ld[6] if side else ld[5]
        bsd = ld[5] if side else ld[6]
        fsi = m.sides[fsd][5] if fsd != 0xFFFF else -1
        bsi = m.sides[bsd][5] if bsd != 0xFFFF else -1
        fs = m.sectors[fsi] if fsi >= 0 else None
        bs = m.sectors[bsi] if bsi >= 0 else None
        return fs, bs, m.sides[fsd], ld, fsi, bsi

    def point_sector(self, px, py):
        # Doom's R_PointOnSide: side>0 means the point is on the RIGHT (front)
        # of the partition vector, which is children[0] == nd[-2].
        m = self.lvl
        n = len(m.nodes) - 1
        while not (n & 0x8000):
            nd = m.nodes[n]
            side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
            n = nd[-2] if side > 0 else nd[-1]
        fs = self.seg_sectors(m.segs[m.ssectors[n & 0x7FFF][1]])[0]
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
        # Per-column occlusion history for the sprite pass: (depth, ct, cb)
        # meaning "behind this depth only rows ct..cb are visible".  Appended
        # front-to-back (the walk order guarantees it).
        clips = [[] for _ in range(RW)]
        masked = []                        # (depth, 'M' op) two-sided midtextures
        out = []
        verts = m.verts
        segs = m.segs
        skyc = m.skyc
        C_CEIL = (84, 88, 104); C_FLOOR = (120, 100, 74); C_SKY = (96, 120, 170)
        # Per-column ray directions (unit forward component) for the flat
        # mapper, and the cylindrical sky texel column (1024 texels per turn,
        # Doom's ANGLETOSKYSHIFT convention).
        dcx = [0.0] * RW; dcy = [0.0] * RW; skyu = [0] * RW
        a2sky = 1024.0 / (2.0 * math.pi)
        for x in range(RW):
            k = ((x + 0.5) - RW / 2) / scale
            dcx[x] = cos_a + k * sin_a
            dcy[x] = sin_a - k * cos_a
            skyu[x] = int((pa - math.atan(k)) * a2sky) & 255
        sky_t = self.sky
        dvsky = int(100.0 / hh * 256)      # horizon at sky texel row 100
        self.fmeta = (px, py, hh)

        def col_rgb(base, f):
            return rgb(int(base[0]*f), int(base[1]*f), int(base[2]*f))

        def rect(cx, y0, y1, color):
            if y1 < y0: return
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            x0 = cx * cw // RW; x1 = (cx + 1) * cw // RW      # exact tiling (no gap)
            out.append(('R', x0, y0, x1 - x0, y1 - y0 + 1, color))

        def flatspan(cx, y0, y1, fgrid, hz, lf):
            if y1 < y0: return
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            x0 = cx * cw // RW; x1 = (cx + 1) * cw // RW
            out.append(('F', x0, x1 - x0, y0, y1 - y0 + 1, fgrid,
                        (hz - viewz) * vsc, dcx[cx], dcy[cx], lf))

        def skyspan(cx, y0, y1):
            if y1 < y0: return
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            x0 = cx * cw // RW; x1 = (cx + 1) * cw // RW
            if sky_t is None:
                out.append(('R', x0, y0, x1 - x0, y1 - y0 + 1, rgb(*C_SKY)))
            else:
                out.append(('W', x0, x1 - x0, y0, y1 - y0 + 1, sky_t[2],
                            sky_t[0], sky_t[1], skyu[cx], y0 * dvsky, dvsky, 256))

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

        state = [RW]                      # open (not yet closed) column count

        def draw_seg(seg):
            fs, bs, fsd, ld, fsi, bsi = self.seg_sectors(seg)
            if fs is None: return
            ax, ay = verts[seg[0]]; bx, by = verts[seg[1]]
            wk = 115 if abs(bx - ax) > abs(by - ay) else 88     # /100 contrast
            d1x = ax - px; d1y = ay - py
            d2x = bx - px; d2y = by - py
            depth1 = d1x * cos_a + d1y * sin_a
            depth2 = d2x * cos_a + d2y * sin_a
            # screen-right axis is (sin a, -cos a): angles grow CCW in Doom,
            # so what's clockwise of the view direction lands on the right.
            sx1 = d1x * sin_a - d1y * cos_a
            sx2 = d2x * sin_a - d2y * cos_a
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
            flags = ld[2]
            mid_t = self.tex.get(_texname(fsd[4]))
            up_t = self.tex.get(_texname(fsd[2]))
            lo_t = self.tex.get(_texname(fsd[3]))
            twosided = bs is not None
            # Texture pegging (Doom R_StoreWallRange): v_top is the texel row at
            # the TOP of each wall piece; anchors move with the linedef flags.
            v_mid = yoff
            if (flags & ML_DONTPEGBOT) and mid_t:
                v_mid = yoff + (ff + mid_t[1] - fc)      # bottom texel row at floor
            csky = skyc[fsi]
            bothsky = False
            if twosided:
                bc = bs[1]; bf = bs[0]
                bothsky = csky and skyc[bsi]     # sky joins across the portal
                v_up = yoff
                if not (flags & ML_DONTPEGTOP) and up_t:
                    v_up = yoff + (bc + up_t[1] - fc)    # bottom texel row at back ceil
                v_lo = yoff + ((fc - bf) if (flags & ML_DONTPEGBOT) else 0)
            cgrid = None if csky else tex.flat(fs[3])
            fgrid = tex.flat(fs[2])
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
                if twosided:
                    ybc = int(hh - (bc - viewz) * vsc * invd)
                    ybf = int(hh - (bf - viewz) * vsc * invd)
                    if bothsky and ybc > yfc:
                        yfc = ybc      # sky continues down to the back ceiling
                if yfc > ct:
                    if csky:
                        skyspan(x, ct, (yfc - 1) if (yfc - 1) < cb else cb)
                    elif cgrid is not None:
                        flatspan(x, ct, (yfc - 1) if (yfc - 1) < cb else cb, cgrid, fc, lf)
                    else:
                        rect(x, ct, (yfc - 1) if (yfc - 1) < cb else cb, col_rgb(C_CEIL, lf*0.9))
                if yff < cb:
                    if fgrid is not None:
                        flatspan(x, (yff + 1) if (yff + 1) > ct else ct, cb, fgrid, ff, lf)
                    else:
                        rect(x, (yff + 1) if (yff + 1) > ct else ct, cb, col_rgb(C_FLOOR, lf*0.9))
                if not twosided:
                    wall(x, yfc, yff, ct, cb, mid_t, u, v_mid, dv, shw, (200, 188, 170))
                    ceilc[x] = 1; floorc[x] = 0
                    state[0] -= 1
                    clips[x].append((dist, 1, 0))
                else:
                    newct = ct if ct > yfc else yfc
                    newcb = cb if cb < yff else yff
                    if mid_t is not None:
                        # masked midtexture: drawn in the sprite pass, clipped
                        # to the opening and window as they are RIGHT NOW
                        yot = yfc if yfc > ybc else ybc
                        yob = yff if yff < ybf else ybf
                        tw_m, th_m, grid_m = mid_t
                        if flags & ML_DONTPEGBOT:
                            zt = (ff if ff > bf else bf) + th_m
                        else:
                            zt = fc if fc < bc else bc
                        ytt = hh - (zt - viewz) * vsc * invd
                        my0 = int(ytt)
                        if my0 < yot: my0 = yot
                        if my0 < ct: my0 = ct
                        if my0 < 0: my0 = 0
                        my1 = int(ytt + th_m / dv)
                        if my1 > yob: my1 = yob
                        if my1 > cb: my1 = cb
                        if my1 > ch - 1: my1 = ch - 1
                        if my1 >= my0:
                            x0m = x * cw // RW; x1m = (x + 1) * cw // RW
                            v0m = int((yoff + (my0 - ytt) * dv) * 256)
                            masked.append((dist, ('M', x0m, x1m - x0m, my0,
                                           my1 - my0 + 1, grid_m, tw_m, th_m,
                                           int(u), v0m, int(dv * 256), shw)))
                    if bc < fc and not bothsky:
                        if up_t:
                            wall(x, yfc, ybc - 1, ct, cb, up_t, u, v_up, dv, shw, (172, 150, 122))
                        else:
                            rect(x, yfc if yfc > ct else ct, (ybc-1) if (ybc-1) < cb else cb, col_rgb(C_CEIL, lf*df))
                        if ybc > newct: newct = ybc
                    if bf > ff:
                        if lo_t:
                            wall(x, ybf + 1, yff, ct, cb, lo_t, u, v_lo, dv, shw, (140, 150, 170))
                        else:
                            rect(x, (ybf+1) if (ybf+1) > ct else ct, yff if yff < cb else cb, col_rgb(C_FLOOR, lf*df))
                        if ybf < newcb: newcb = ybf
                    ceilc[x] = newct; floorc[x] = newcb
                    if newct > newcb:
                        state[0] -= 1
                    if newct > ct or newcb < cb:
                        clips[x].append((dist, newct, newcb))

        def bbox_vis(top, bot, left, right):
            # Conservative frustum test: skip the far child only when all 4
            # corners are behind the near plane or project past one screen
            # edge, or every covered column is already closed.
            all_behind = True
            lo = 1e9; hi = -1e9
            straddle = False
            for (bx, by) in ((left, top), (right, top), (left, bot), (right, bot)):
                dx = bx - px; dy = by - py
                depth = dx * cos_a + dy * sin_a
                sx = dx * sin_a - dy * cos_a
                if depth >= NEAR:
                    all_behind = False
                    r = (RW / 2) + (sx / depth) * scale
                    if r < lo: lo = r
                    if r > hi: hi = r
                else:
                    straddle = True
            if all_behind:
                return False
            if not straddle:
                if hi < 0 or lo > RW - 1:
                    return False
                i0 = 0 if lo < 0 else int(lo)
                i1 = RW - 1 if hi > RW - 1 else int(hi) + 1
                if i1 > RW - 1: i1 = RW - 1
                for x in range(i0, i1 + 1):
                    if ceilc[x] <= floorc[x]:
                        return True
                return False
            return True

        def walk(n):
            # Front-to-back: descend the child the viewer is in first, then
            # the far child (bbox-culled); stop once every column is closed.
            while state[0] > 0:
                if n & 0x8000:
                    ss = m.ssectors[n & 0x7FFF]
                    first = ss[1]
                    for i in range(first, first + ss[0]):
                        draw_seg(segs[i])
                    return
                nd = m.nodes[n]
                side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
                if side > 0:
                    walk(nd[-2])
                    if not bbox_vis(nd[8], nd[9], nd[10], nd[11]): return
                    n = nd[-1]
                else:
                    walk(nd[-1])
                    if not bbox_vis(nd[4], nd[5], nd[6], nd[7]): return
                    n = nd[-2]

        walk(len(m.nodes) - 1)
        for x in range(RW):
            if ceilc[x] <= floorc[x]:      # leftover openings show sky
                skyspan(x, ceilc[x], floorc[x])

        # ---- things: project, sort far-to-near with the masked midtextures,
        # clip each column against the wall silhouette history --------------
        vis = []
        for t in self.things_live:
            dxt = t[0] - px; dyt = t[1] - py
            depth = dxt * cos_a + dyt * sin_a
            if depth < NEAR:
                continue
            vis.append((depth, 1, t, dxt, dyt))
        for (d, op) in masked:
            vis.append((d, 0, op, 0.0, 0.0))
        vis.sort(key=lambda e: -e[0])
        spr = self.spr
        HPI = math.pi; A202 = 3.5342917    # 202.5 degrees
        for (depth, kind, t, dxt, dyt) in vis:
            if kind == 0:
                out.append(t)              # pre-built masked midtex column
                continue
            frames = t[5]
            fch = frames[t[8] % len(frames):t[8] % len(frames) + 1]
            # Doom: rot from (viewer->thing angle) - thing facing + 202.5 deg
            rel = (math.atan2(dyt, dxt) - t[2] + A202) % 6.2831853
            s = spr.get(t[4], fch, int(rel / 0.7853982) & 7)
            if s is None:
                continue
            w, h, lo, to, grid, mirror = s
            invd = 1.0 / depth
            scr = scale * invd             # screen cols per world unit
            sxc = dxt * sin_a - dyt * cos_a
            rxl = (RW / 2) + (sxc - lo) * scr
            du = depth / scale
            sec = t[7]
            ztop = sec[1] if t[6] else (sec[0] + to)
            ytt = hh - (ztop - viewz) * vsc * invd
            dv = depth / vsc
            iy0 = int(ytt); iy1 = int(ytt + h / dv) - 1
            if iy1 < 0 or iy0 > ch - 1:
                continue
            ix0 = int(rxl); ix1 = int(rxl + w * scr)
            if ix0 < 0: ix0 = 0
            if ix1 > RW - 1: ix1 = RW - 1
            df = 1200.0 / (depth + 650.0)
            if df > 1.0: df = 1.0
            elif df < 0.68: df = 0.68
            sh = int((0.72 + 0.28 * (sec[4] / 255.0)) * df * 256)
            dvfp = int(dv * 256)
            for x in range(ix0, ix1 + 1):
                uf = (x + 0.5 - rxl) * du
                if uf < 0 or uf >= w:
                    continue
                texcol = int(uf)
                if mirror:
                    texcol = w - 1 - texcol
                s0 = 0; s1 = ch - 1
                for (d, c0, c1) in clips[x]:
                    if d < depth - 0.01:
                        if c0 > s0: s0 = c0
                        if c1 < s1: s1 = c1
                        if s0 > s1: break
                    else:
                        break
                y0 = iy0 if iy0 > s0 else s0
                y1 = iy1 if iy1 < s1 else s1
                if y0 < 0: y0 = 0
                if y1 > ch - 1: y1 = ch - 1
                if y1 < y0:
                    continue
                x0s = x * cw // RW; x1s = (x + 1) * cw // RW
                out.append(('M', x0s, x1s - x0s, y0, y1 - y0 + 1, grid, w, h,
                            texcol, int((y0 - ytt) * dv * 256), dvfp, sh))
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
        vx, vy, hh = self.fmeta
        for op in self.frame:
            k = op[0]
            if k == 'R':
                cv.fill_rect(op[1], op[2], op[3], op[4], op[5])
            elif k == 'W':
                _, x, wpx, y0, count, grid, tw, th, tc, v0, dv, sh = op
                xx = x
                for _2 in range(wpx):
                    cv.wall_col(xx, y0, count, grid, tw, th, tc, v0, dv, pal, sh)
                    xx += 1
            elif k == 'M':
                _, x, wpx, y0, count, grid, tw, th, tc, v0, dv, sh = op
                xx = x
                for _2 in range(wpx):
                    cv.mask_col(xx, y0, count, grid, tw, th, tc, v0, dv, pal, sh)
                    xx += 1
            else:                                        # 'F': floor/ceiling
                _, x, wpx, y0, count, grid, aa, ddx, ddy, lf = op
                xx = x
                for _2 in range(wpx):
                    cv.flat_col(xx, y0, count, grid, pal, aa, hh, ddx, ddy, vx, vy, lf)
                    xx += 1
        cv.text(6, 6, LEVEL + "  arrows move/turn", rgb(235, 235, 210))

    def tick(self):
        # Return True only when we actually re-rendered this frame, so the shell
        # repaints on movement but idles (16ms sleep) when standing still.
        # Returning False when nothing changed is what stops the 100% CPU busy
        # loop; the error screen was already painted once at window open.
        if self.err is None and self.dirty:
            self.render()
            self.dirty = False
            return True
        return False

    def _blocked(self, nx, ny):
        s = self.point_sector(nx, ny)
        if s is None:
            return True
        cur = self.point_sector(self.px, self.py)
        if cur is not None and s[0] - cur[0] > 24:     # Doom's max step height
            return True
        if s[1] - s[0] < 56:
            return True
        return False

    def key(self, uni, scan, ctrl):
        if self.err is not None:
            return False
        ca = math.cos(self.pa); sa = math.sin(self.pa)
        nx, ny = self.px, self.py
        # UnoDOS arrow scancodes: Up=1 Down=2 Right=3 Left=4.
        # Angles grow counter-clockwise (Doom BAMs), so Left = pa increasing.
        if scan == 0x04:
            self.pa += TURN; self.dirty = True; return True
        if scan == 0x03:
            self.pa -= TURN; self.dirty = True; return True
        if scan == 0x01:
            nx = self.px + ca * MOVE; ny = self.py + sa * MOVE
        elif scan == 0x02:
            nx = self.px - ca * MOVE; ny = self.py - sa * MOVE
        elif uni == ord(','):
            nx = self.px - sa * MOVE; ny = self.py + ca * MOVE
        elif uni == ord('.'):
            nx = self.px + sa * MOVE; ny = self.py - ca * MOVE
        else:
            return False
        if not self._blocked(nx, ny):
            self.px = nx; self.py = ny
        self.dirty = True
        return True


app = Duum()
