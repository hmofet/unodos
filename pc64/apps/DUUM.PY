# Duum 0.1.0 - GENERATED FILE, DO NOT EDIT.
#
# Built from the duum package by tools/build.py on 2026-08-18.
# Edit the package and rebuild; edits here are lost.
# Source and licence (MPL-2.0): https://github.com/hmofet/duum
#
# UnoDOS build: engine only; the device supplies `uno` and a C canvas.

# duum/engine.py - Duum: a Doom engine written in Python.
#
# The BSP walk, the portal clipping, the perspective-correct texture mapping,
# the thing projection and every line of the game logic below are pure Python.
# There is no native code and no third-party package anywhere in this file: it
# imports struct and math from the standard library, and nothing else.
#
# What this file does NOT do is write pixels.  render() builds a display list
# of spans and draw() replays it into a Canvas; duum/raster.py holds the
# reference Canvas, also pure Python.  That is the seam a port replaces to go
# fast - UnoDOS hands in a C canvas, a desktop can hand in anything it likes -
# and the engine does not change either way.
#
# The other seam is `uno` (see duum/hostapi.py): file I/O, a clock, and an
# optional key-state read.  That is the entire platform surface.
#
# Controls: Up/Down move, Left/Right turn, comma/period strafe, space opens
# and uses, ctrl fires, 1-7 pick a weapon.

import uno

import struct
import math

WADNAME = "DOOM1.WAD"
LEVEL = "E1M1"

FOV = math.pi / 2
# Near clip.  Duum's collision is sector-based (no linedef-radius test), so
# unlike vanilla P_TryMove the player CAN press right up against a wall; at
# 4.0 the whole seg fell inside the clip and the wall vanished, showing the
# void through it whenever you walked into a corner.  1.0 still keeps the
# projection well conditioned.
NEAR = 1.0
# Conservative world-space half-width for the thing frustum cull: no Doom
# sprite reaches this far from its origin, so the cull never drops a thing
# that would have drawn.
# Distance bounds where the light falloff 1200/(dist+650) saturates at
# its 1.0 and 0.68 clamps.  Derived, not tuned: keep them with the curve.
DF_NEAR = 1200.0 - 650.0
DF_FAR = 1200.0 / 0.68 - 650.0
SPR_HW = 512.0
EYE = 41
VER = 1.2          # Doom's 320x200-stretched-to-4:3 pixel aspect
MOVSPD = 320.0     # units/s (Doom run speed ballpark)
TURNSPD = 3.1      # rad/s

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
                b"SSECTORS", b"NODES", b"SECTORS", b"THINGS", b"REJECT")
        lp = {}
        for j in range(mi + 1, mi + 12):
            nm = wad.dir[j][0]
            if nm in want:
                lp[nm] = wad.lump_at(j)
        self.verts    = rows(lp[b"VERTEXES"], "<hh", 4)
        # lines, sides and sectors are LISTS: one-shot specials go dead,
        # switches flip textures, doors/lifts mutate heights live
        self.lines    = [list(t) for t in rows(lp[b"LINEDEFS"], "<HHHHHHH", 14)]
        self.sides    = [list(t) for t in rows(lp[b"SIDEDEFS"], "<hh8s8s8sH", 30)]
        self.segs     = rows(lp[b"SEGS"], "<HHhHHh", 12)
        self.ssectors = rows(lp[b"SSECTORS"], "<HH", 4)
        self.nodes    = rows(lp[b"NODES"], "<hhhh8hHH", 28)
        self.sectors  = [list(t) for t in rows(lp[b"SECTORS"], "<hh8s8shHH", 26)]
        self.things   = rows(lp[b"THINGS"], "<hhHHH", 10)
        self.skyc     = [s[3][:6] == b"F_SKY1" for s in self.sectors]
        self.reject   = lp.get(b"REJECT", b"")
        ns = len(self.sectors)
        self.nsec = ns
        adj = [[] for _ in range(ns)]      # sector -> neighbor sector indices
        for ld in self.lines:
            s1 = self.sides[ld[5]][5] if ld[5] != 0xFFFF else -1
            s2 = self.sides[ld[6]][5] if ld[6] != 0xFFFF else -1
            if s1 >= 0 and s2 >= 0 and s1 != s2:
                if s2 not in adj[s1]: adj[s1].append(s2)
                if s1 not in adj[s2]: adj[s2].append(s1)
        self.adj = adj
        self.tagsec = {}
        for si in range(ns):
            tg = self.sectors[si][6]
            if tg:
                self.tagsec.setdefault(tg, []).append(si)
        # trigger lines a move can cross (walkover specials)
        self.trig = [i for i in range(len(self.lines))
                     if self.lines[i][3] in ACT and ACT[self.lines[i][3]][1]]

    def rejects(self, a, b):
        """True if REJECT says sector a can never see sector b."""
        if a < 0 or b < 0:
            return False
        i = a * self.nsec + b
        byte = i >> 3
        if byte >= len(self.reject):
            return False
        return (self.reject[byte] >> (i & 7)) & 1

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
        self.mcache = {}
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

    def getm(self, name):
        """get() for MASKED use (two-sided midtextures drawn with mask_span):
        texels no patch covers become the 0xFF transparent sentinel, and real
        palette index 255 is remapped to 254 like sprites - so the holes in a
        grating show what is behind them instead of opaque palette color 0."""
        if name is None:
            return None
        name = name.upper()
        t = self.mcache.get(name)
        if t is not None or name in self.mcache:
            return t
        td = self.defs.get(name)
        if td is None:
            self.mcache[name] = None
            return None
        w, h, pats = td
        grid = bytearray(b"\xff" * (w * h))
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
                            v = pix[i]
                            grid[base + dy] = 254 if v == 255 else v
        t = (w, h, grid)
        self.mcache[name] = t
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

    @staticmethod
    def decode(d):
        """Doom picture lump -> (w, h, leftoffs, topoffs, grid) with 0xFF
        transparent (real index 255 remapped to 254)."""
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
        return (w, h, lo, to, grid)

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
            s = self.decode(self.wad.lump_at(li))
            self.cache[li] = s
        return (s[0], s[1], s[2], s[3], s[4], mirror)

    def ui(self, name):
        """Any picture lump by name (STBAR, STTNUM3, psprites...)."""
        s = self.cache.get(name)
        if s is not None or name in self.cache:
            return s
        d = self.wad.lump(name)
        s = self.decode(d) if len(d) >= 8 else None
        self.cache[name] = s
        return s


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


# ---- gameplay tables -------------------------------------------------------
# monsters: type -> (hp, speed u/s, radius, painchance/255, attack kind)
# attack: 0 melee, 1 hitscan x1, 2 hitscan x3 (shotgunner), 3 imp ball,
#         4 caco ball, 5 baron ball, 6 skull charge
MONST = {
    3004: (20, 90, 20, 200, 1), 9: (30, 90, 20, 170, 2),
    3001: (60, 90, 20, 200, 3), 3002: (150, 145, 30, 180, 0),
    3005: (400, 100, 31, 128, 4), 3003: (1000, 100, 24, 50, 5),
    3006: (100, 200, 16, 256, 6),
}
# sprite family -> (walk, attack, pain, death) frame letters
MFRAMES = {
    b"POSS": (b"ABCD", b"EF", b"G", b"HIJKL"),
    b"SPOS": (b"ABCD", b"EF", b"G", b"HIJKL"),
    b"TROO": (b"ABCD", b"EFG", b"H", b"IJKLM"),
    b"SARG": (b"ABCD", b"EFG", b"H", b"IJKLMN"),
    b"HEAD": (b"A",    b"BCD", b"E", b"FGHIJK"),
    b"BOSS": (b"ABCD", b"EFG", b"H", b"IJKLMNO"),
    b"SKUL": (b"AB",   b"CD",  b"E", b"FGHIJK"),
}
# weapons: slot -> (name, psprite, flash, refire s, ammo kind, per shot)
# ammo kinds: 0 none, 1 bullets, 2 shells, 3 rockets, 4 cells
WEAP = {
    1: (b"Fist",     b"PUNG", None,    0.55, 0, 0),
    2: (b"Pistol",   b"PISG", b"PISF", 0.42, 1, 1),
    3: (b"Shotgun",  b"SHTG", b"SHTF", 1.05, 2, 1),
    4: (b"Chaingun", b"CHGG", b"CHGF", 0.12, 1, 1),
    5: (b"Rockets",  b"MISG", b"MISF", 1.00, 3, 1),
    6: (b"Plasma",   b"PLSG", b"PLSF", 0.10, 4, 1),
}
AMMOMAX = (0, 200, 50, 50, 300)

# pickups: type -> (kind, a, b, message)
#  kind: 0 health(a, cap b) 1 armor-set(a) 2 armor-bonus 3 ammo(kind a, n b)
#  4 weapon(slot a, ammo kind, n) 5 key(bit a) 6 backpack 7 powerup msg-only
PICKUP = {
    2011: (0, 10, 100, "Picked up a stimpack."),
    2012: (0, 25, 100, "Picked up a medikit."),
    2014: (0, 1, 200, "Picked up a health bonus."),
    2013: (0, 100, 200, "Supercharge!"),
    2015: (2, 1, 200, "Picked up an armor bonus."),
    2018: (1, 100, 0, "Picked up the armor."),
    2019: (1, 200, 0, "Picked up the MegaArmor!"),
    2007: (3, 1, 10, "Picked up a clip."),
    2048: (3, 1, 50, "Picked up a box of bullets."),
    2008: (3, 2, 4, "Picked up 4 shotgun shells."),
    2049: (3, 2, 20, "Picked up a box of shotgun shells."),
    2010: (3, 3, 1, "Picked up a rocket."),
    2046: (3, 3, 5, "Picked up a box of rockets."),
    2047: (3, 4, 20, "Picked up an energy cell."),
    8:    (6, 0, 0, "Picked up a backpack full of ammo!"),
    2001: (4, 3, 2, "You got the shotgun!"),
    2002: (4, 4, 1, "You got the chaingun!"),
    2003: (4, 5, 3, "You got the rocket launcher!"),
    2004: (4, 6, 4, "You got the plasma gun!"),
    5:    (5, 1, 0, "Picked up a blue keycard."),
    13:   (5, 2, 0, "Picked up a red keycard."),
    6:    (5, 4, 0, "Picked up a yellow keycard."),
    2026: (7, 0, 0, "Computer area map."),
    2045: (7, 0, 0, "Light amplification visor."),
    2024: (7, 0, 0, "Partial invisibility!"),
    2022: (7, 0, 0, "Invulnerability!"),
    2023: (0, 100, 100, "Berserk!"),
    2025: (7, 0, 0, "Radiation shielding suit."),
}

# door/action linedef specials (the E1 working set).
# use-doors (back sector is the door): type -> (key bits needed, stay open)
DOOR_USE = {1: (0, 0), 26: (1, 0), 27: (4, 0), 28: (2, 0),
            31: (0, 1), 32: (1, 1), 33: (2, 1), 34: (4, 1),
            117: (0, 0), 118: (0, 1)}
# tagged actions: type -> (kind, walkover?)  kind: 0 door open/close, 1 door
# open stay, 2 lift, 3 floor to lowest neighbor, 4 exit, 5 secret exit,
# 6 teleport, 7 door close
ACT = {
    2: (1, 1), 3: (7, 1), 4: (0, 1), 16: (7, 1), 42: (7, 0), 61: (1, 0),
    63: (0, 0), 86: (1, 1), 90: (0, 1), 103: (1, 0), 105: (0, 1),
    106: (1, 1), 107: (7, 1), 109: (1, 1), 110: (7, 1), 112: (0, 0),
    113: (1, 0), 114: (7, 0),
    10: (2, 1), 21: (2, 0), 62: (2, 0), 88: (2, 1), 120: (2, 1), 121: (2, 1),
    123: (2, 0), 122: (2, 0),
    19: (3, 1), 36: (3, 1), 37: (3, 1), 38: (3, 1), 82: (3, 1), 83: (3, 1),
    98: (3, 1),
    11: (4, 0), 52: (4, 1), 51: (5, 0), 124: (5, 1),
    39: (6, 1), 97: (6, 1), 125: (6, 1),
}

EP_ORDER = ["E1M1", "E1M2", "E1M3", "E1M4", "E1M5", "E1M6", "E1M7", "E1M8",
            "E1M9"]
SECRET_MAP = {"E1M3": "E1M9"}
AFTER_SECRET = {"E1M9": "E1M4"}

# decoration types that block movement (tuple: this MicroPython build has
# no frozenset, and 25 entries scan fast enough)
SOLID_DECOR = (25, 26, 27, 28, 29, 30, 31, 32, 33, 35, 36, 37,
               41, 42, 43, 44, 45, 46, 47, 48, 54, 55, 56, 57, 2028)

# W1/S1 specials fire once, then the line goes dead
ONE_SHOT = (2, 3, 4, 10, 11, 16, 19, 21, 36, 37, 38, 39, 51, 52,
            103, 109, 110, 112, 113, 114, 121, 122, 124, 125)


class Mover:
    """One moving sector plane.  kind: 'door' (ceil up, wait, close),
    'open' (ceil up, stays), 'close' (ceil down, stays), 'lift' (floor down,
    wait, back up), 'floor' (floor to target, stays)."""
    DOORSPD = 170.0; LIFTSPD = 200.0; FLOORSPD = 90.0

    def __init__(self, kind, si, sec, target):
        self.kind = kind
        self.si = si
        self.sec = sec
        self.target = target
        self.phase = 0
        self.wait = 0.0
        self.orig = sec[0]                  # lift return height

    def tick(self, dt):
        """-> False when finished."""
        k = self.kind
        sec = self.sec
        if k == 'door' or k == 'open':
            if self.phase == 0:
                sec[1] += Mover.DOORSPD * dt
                if sec[1] >= self.target:
                    sec[1] = self.target
                    if k == 'open':
                        return False
                    self.phase = 1
                    self.wait = 4.0
            elif self.phase == 1:
                self.wait -= dt
                if self.wait <= 0:
                    self.phase = 2
            else:
                sec[1] -= Mover.DOORSPD * dt
                if sec[1] <= sec[0]:
                    sec[1] = sec[0]
                    return False
        elif k == 'close':
            sec[1] -= Mover.DOORSPD * dt
            if sec[1] <= sec[0]:
                sec[1] = sec[0]
                return False
        elif k == 'lift':
            if self.phase == 0:
                sec[0] -= Mover.LIFTSPD * dt
                if sec[0] <= self.target:
                    sec[0] = self.target
                    self.phase = 1
                    self.wait = 3.0
            elif self.phase == 1:
                self.wait -= dt
                if self.wait <= 0:
                    self.phase = 2
            else:
                sec[0] += Mover.LIFTSPD * dt
                if sec[0] >= self.orig:
                    sec[0] = self.orig
                    return False
        else:                               # 'floor'
            if sec[0] < self.target:
                sec[0] += Mover.FLOORSPD * dt
                if sec[0] >= self.target:
                    sec[0] = self.target
                    return False
            else:
                sec[0] -= Mover.FLOORSPD * dt
                if sec[0] <= self.target:
                    sec[0] = self.target
                    return False
        return True


# ---- the engine ------------------------------------------------------------
class Duum(uno.App):

    def build(self, cv):
        self.err = None
        self.frame = []
        self.cv = cv          # singleton canvas; render() emits through it
        self.cw = cv.width()
        self.ch = cv.height()
        try:
            self.wad = Wad(WADNAME)
            self.tex = Textures(self.wad)
            self.pal = self.tex.pal
            self.spr = Sprites(self.wad)
            self.have_ticks = hasattr(uno, "ticks")
            self.have_keys = hasattr(uno, "keys_down")
            self.kd = 0; self.kd_prev = 0
            self.rng = 0x29A5D1
            self.new_game(LEVEL)
        except Exception as e:
            self.err = repr(e)

    def rnd(self):
        self.rng = (self.rng * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.rng >> 16) & 0xFF

    def snd(self, midi, ticks):
        try:
            uno.beep(midi, ticks)
        except Exception:
            pass

    # ---- game state ---------------------------------------------------------
    def new_game(self, level):
        self.health = 100; self.armor = 0
        self.ammo = [0, 50, 0, 0, 0]
        self.maxammo = list(AMMOMAX)
        self.owned = {1: 1, 2: 1}
        self.weapon = 2
        self.keys = 0
        self.invuln_until = 0.0; self.suit_until = 0.0; self.berserk = False
        self.now = 0.0
        self.last_ticks = None
        self.load_level(level)

    def load_level(self, level):
        self.level = level
        self.lvl = Level(self.wad, level)
        self.sky = self.tex.get("SKY" + level[1])   # E<n> -> SKY<n>
        px, py, pa = self.lvl.player_start()
        self.px = float(px); self.py = float(py)
        self.pa = math.radians(pa)
        self.held = {}
        self.use_press = False
        self.movers = []
        self.fx = []
        self.proj = []
        self.msg = ""; self.msg_until = 0.0
        self.refire_at = 0.0
        self.wflash_until = 0.0
        self.bob = 0.0; self.bobamp = 0.0
        self.dead = False
        self.finish = None            # (next level or None, entered-at)
        self.dmg_next = 0.0
        self.e1m8_done = False
        self.mon_active = False
        self.anim_next = 0.0
        self.hud_key = None; self.hud_ops = []
        self.psec_i = self.point_secidx(self.px, self.py)
        self.segrec = None            # per-seg render records, built lazily
        self.colgeo = None            # viewer-independent per-column tables
        self.sssec = None             # subsector -> sector index
        self.seen = None              # per-sector 'walk reached it' stamps
        self.stamp = 0
        self.load_things()
        self.dirty = True

    # live thing record layout:
    # [0]x [1]y [2]facing [3]type [4]sprite [5]frames [6]hang [7]sector(list)
    # [8]frame index [9]hp [10]state (0 idle 1 chase 2 attack 3 pain 4 dying
    # 5 dead/corpse) [11]state timer [12]solid radius [13]think clock
    # [14]sector index [15]attack cooldown
    def load_things(self):
        self.things_live = []
        for (x, y, ang, typ, fl) in self.lvl.things:
            if fl & 16:                      # multiplayer-only
                continue
            if not (fl & 2):                 # skill flags: play Hurt-Me-Plenty
                continue
            info = THINGTAB.get(typ)
            if info is None:
                continue
            si = self.point_secidx(x, y)
            if si < 0:
                continue
            t = [float(x), float(y), math.radians(ang), typ, info[0], info[1],
                 info[2], self.lvl.sectors[si], 0,
                 0, 0, 0.0, 0, (self.rnd() & 31) / 32.0, si, 0.0]
            ms = MONST.get(typ)
            if ms:
                t[9] = ms[0]; t[12] = ms[2]
            elif typ == 2035:
                t[9] = 20; t[12] = 10
            elif typ in SOLID_DECOR:
                t[12] = 16
            self.things_live.append(t)

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

    # Per-seg RENDER RECORD, built once per level instead of per seg per frame.
    # render() used to re-derive all of this on every seg of every frame:
    # seg_sectors(), three _texname() calls (each a bytes.find + a .decode,
    # so a fresh str), three Textures.get() dict probes, two flat() probes and
    # a math.sqrt - about 405 string builds and 2,700 dict lookups per frame
    # for data that cannot change between frames.
    #
    # What is STATIC (here) vs DYNAMIC (still read per frame in draw_seg):
    #   static  - geometry, wall length, u offset, fake contrast, the sector
    #             LIST OBJECTS, texture tuples, flat grids, sector light
    #   dynamic - floor/ceiling HEIGHTS, which doors and lifts mutate in place
    #             on those same list objects, so holding the list is correct
    # Switches are the one thing that edits a sidedef's texture at run time
    # (flip_switch), so that path drops the cache and it rebuilds lazily.
    R_AX, R_AY, R_BX, R_BY, R_WK, R_WLEN, R_U1 = 0, 1, 2, 3, 4, 5, 6
    R_FS, R_BS, R_YOFF, R_FLAGS = 7, 8, 9, 10
    R_MID, R_UP, R_LO, R_CSKY, R_BSKY, R_CGRID, R_FGRID, R_LF = \
        11, 12, 13, 14, 15, 16, 17, 18

    # Subsector -> sector index, built once per level.  render() uses it to
    # mark which sectors the BSP walk actually reached, which is what makes
    # the thing pass affordable: a thing in a sector the walk never entered
    # is behind something opaque, off the frustum, or past the point where
    # every column had closed - in all three cases it cannot draw.  This is
    # Doom's own rule (it adds sprites per subsector as it renders them),
    # applied a level coarser so it stays conservative.
    def ss_sectors(self):
        if self.sssec is not None:
            return self.sssec
        m = self.lvl
        out = []
        for ss in m.ssectors:
            si = -1
            for i in range(ss[1], ss[1] + ss[0]):
                si = self.seg_sectors(m.segs[i])[4]
                if si >= 0:
                    break
            out.append(si)
        self.sssec = out
        return out

    def seg_recs(self):
        if self.segrec is not None:
            return self.segrec
        m = self.lvl
        tex = self.tex
        verts = m.verts
        out = []
        for seg in m.segs:
            fs, bs, fsd, ld, fsi, bsi = self.seg_sectors(seg)
            if fs is None:
                out.append(None)
                continue
            ax, ay = verts[seg[0]]
            bx, by = verts[seg[1]]
            # vanilla fake contrast: N-S lighter, E-W darker, diagonals flat
            wk = 88 if ay == by else 115 if ax == bx else 100
            dx = bx - ax; dy = by - ay
            wlen = math.sqrt(dx * dx + dy * dy)
            twosided = bs is not None
            out.append((
                ax, ay, bx, by, wk, wlen, seg[5] + fsd[0],
                fs, bs, fsd[1], ld[2],
                (tex.getm if twosided else tex.get)(_texname(fsd[4])),
                tex.get(_texname(fsd[2])),
                tex.get(_texname(fsd[3])),
                m.skyc[fsi],
                (bsi >= 0 and m.skyc[bsi]),
                None if m.skyc[fsi] else tex.flat(fs[3]),
                tex.flat(fs[2]),
                0.72 + 0.28 * (fs[4] / 255.0),
            ))
        self.segrec = out
        return out

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

    def point_secidx(self, px, py):
        m = self.lvl
        n = len(m.nodes) - 1
        while not (n & 0x8000):
            nd = m.nodes[n]
            side = (px - nd[0]) * nd[3] - (py - nd[1]) * nd[2]
            n = nd[-2] if side > 0 else nd[-1]
        return self.seg_sectors(m.segs[m.ssectors[n & 0x7FFF][1]])[4]

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
        hwf = RW / 2                                 # screen centre, column space
        vsc = ((cw / 2) / math.tan(FOV / 2)) * VER   # canvas-px vertical scale
        inv_vsc = 1.0 / vsc                          # dv is a multiply, not a divide
        hh = ch / 2
        # internal column -> canvas pixel edge, once per frame instead of
        # twice per emitted op (exact tiling, so no seams)
        # Per-column geometry that does not depend on where the viewer is:
        # the column -> canvas edge table, the ray slope k, and atan(k) for
        # the sky.  All three are functions of cw and RW alone, and rebuilding
        # them per frame cost 220 divisions and 220 atan calls for values
        # that had not changed.  The scratch arrays ride along so the frame
        # overwrites them instead of allocating three fresh lists.
        geo = self.colgeo
        if geo is None or geo[0] != cw or geo[1] != RW:
            geo = self.colgeo = (
                cw, RW,
                [(i * cw) // RW for i in range(RW + 1)],
                [((i + 0.5) - RW / 2) / scale for i in range(RW)],
                [((i + 1) * cw) // RW - (i * cw) // RW for i in range(RW)],
                [math.atan(((i + 0.5) - RW / 2) / scale) for i in range(RW)],
                [0.0] * RW, [0.0] * RW, [0] * RW)
        colx = geo[2]; kcol = geo[3]; colw_ = geo[4]; atank = geo[5]
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
        recs = self.seg_recs()
        C_CEIL = (84, 88, 104); C_FLOOR = (120, 100, 74); C_SKY = (96, 120, 170)
        # Per-column ray directions (unit forward component) for the flat
        # mapper, and the cylindrical sky texel column (1024 texels per turn,
        # Doom's ANGLETOSKYSHIFT convention).
        dcx = geo[6]; dcy = geo[7]; skyu = geo[8]
        a2sky = 1024.0 / (2.0 * math.pi)
        for x in range(RW):
            k = kcol[x]
            dcx[x] = cos_a + k * sin_a
            dcy[x] = sin_a - k * cos_a
            sv = (pa - atank[x]) * a2sky
            iv = int(sv)
            if sv < iv:
                iv -= 1                # floor(): int() truncates toward zero,
            skyu[x] = iv & 255         # which seams the sky at the 0 wrap
        sky_t = self.sky
        dvsky = int(100.0 / hh * 256)      # horizon at sky texel row 100
        self.fmeta = (px, py, hh)
        # ---- the rasteriser -------------------------------------------------
        # PURE PYTHON, deliberately.  This is the hottest code in the app and
        # an earlier pass moved it into C (cv.seg_cols, upy_port/mod_uno.c);
        # that C is kept as a reference transcription but is no longer called.
        # "The geometry and the game logic are Python" is a claim this project
        # makes out loud, and this loop is where it has to be true.
        #
        # What buys the speed back is arrangement rather than a compiler.  Two
        # rules do most of the work: every emitter is INLINED into the column
        # loop, because a Python call plus the attribute lookups behind it cost
        # more than the span it emits; and everything that does not vary per
        # column is hoisted into a local before the loop starts.
        sky_rgb = rgb(*C_SKY)

        def col_rgb(base, f):
            return rgb(int(base[0]*f), int(base[1]*f), int(base[2]*f))

        def skyspan(cx, y0, y1):
            if y1 < y0: return
            if y0 < 0: y0 = 0
            if y1 > ch - 1: y1 = ch - 1
            x0 = colx[cx]; x1 = colx[cx + 1]
            if sky_t is None:
                out.append(('R', x0, y0, x1 - x0, y1 - y0 + 1, sky_rgb))
            else:
                out.append(('W', x0, x1 - x0, y0, y1 - y0 + 1, sky_t[2],
                            sky_t[0], sky_t[1], skyu[cx], y0 * dvsky, dvsky, 256))

        # Which sectors the walk reaches.  A stamp counter rather than a
        # cleared array: comparing against this frame's stamp is the same
        # test as a fresh bytearray without touching every sector each frame.
        sssec = self.ss_sectors()
        seen = self.seen
        if seen is None or len(seen) != len(m.sectors):
            seen = self.seen = [0] * len(m.sectors)
            self.stamp = 0
        stamp = self.stamp = self.stamp + 1

        state = [RW]                      # open (not yet closed) column count

        def draw_seg(si):
            rec = recs[si]
            if rec is None: return
            # One unpack beats nineteen subscripts, and it is cheaper even
            # on the segs that turn round at the near-plane test below.
            (ax, ay, bx, by, wk, wlen, ru1, fs, bs, yoff, flags,
             mid_t, up_t, lo_t, csky, bsky, cgrid, fgrid, lf) = rec
            d1x = ax - px; d1y = ay - py
            d2x = bx - px; d2y = by - py
            depth1 = d1x * cos_a + d1y * sin_a
            depth2 = d2x * cos_a + d2y * sin_a
            # screen-right axis is (sin a, -cos a): angles grow CCW in Doom,
            # so what's clockwise of the view direction lands on the right.
            sx1 = d1x * sin_a - d1y * cos_a
            sx2 = d2x * sin_a - d2y * cos_a
            u1 = ru1; u2 = u1 + wlen
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
            if rx2 < ix2:              # int() truncates toward zero: floor(),
                ix2 -= 1               # or a seg wholly off-screen left
            if ix1 < 0: ix1 = 0        # (rx2 in (-1,0)) claims column 0
            if ix2 > RW - 1: ix2 = RW - 1
            if ix1 > ix2: return
            inv1 = 1.0 / depth1; inv2 = 1.0 / depth2
            uz1 = u1 * inv1; uz2 = u2 * inv2
            fc = fs[1]; ff = fs[0]          # DYNAMIC: doors and lifts move these
            twosided = bs is not None
            # Texture pegging (Doom R_StoreWallRange): v_top is the texel row at
            # the TOP of each wall piece; anchors move with the linedef flags.
            v_mid = yoff
            if (flags & ML_DONTPEGBOT) and mid_t:
                v_mid = yoff + (ff + mid_t[1] - fc)      # bottom texel row at floor
            bothsky = False
            if twosided:
                bc = bs[1]; bf = bs[0]
                bothsky = csky and bsky      # sky joins across the portal
                v_up = yoff
                if not (flags & ML_DONTPEGTOP) and up_t:
                    v_up = yoff + (bc + up_t[1] - fc)    # bottom texel row at back ceil
                v_lo = yoff + ((fc - bf) if (flags & ML_DONTPEGBOT) else 0)
            span = rx2 - rx1
            # Per-seg constants the column loop used to rebuild on EVERY
            # column: the plane-to-eye offsets scaled into screen space, and
            # the per-column slopes of 1/depth and u/depth.  Folding the
            # /span into the slope takes the interpolation from
            # "divide, then multiply" to a single multiply.
            kfc = (fc - viewz) * vsc
            kff = (ff - viewz) * vsc
            if twosided:
                kbc = (bc - viewz) * vsc
                kbf = (bf - viewz) * vsc
            k_invd = (inv2 - inv1) / span
            k_uz = (uz2 - uz1) / span
            # ---- hoists.  Everything below is constant across this seg's
            # columns, and the loop that follows can run a couple of hundred
            # times for one seg, so each hoist is that many bytecode
            # dispatches removed.
            _out = out; _ceilc = ceilc; _floorc = floorc; _clips = clips
            _colx = colx; _colw = colw_
            _dcx = dcx; _dcy = dcy; _skyu = skyu
            _append = out.append
            _masked = masked
            chm1 = ch - 1
            # Texture tuples unpacked once instead of once per column.
            if mid_t is None:
                m_tw = m_th = m_grid = 0
                th_m = 0
            else:
                m_tw = mid_t[0]; m_th = mid_t[1]; m_grid = mid_t[2]
                th_m = m_th
            if up_t is not None:
                u_tw = up_t[0]; u_th = up_t[1]; u_grid = up_t[2]
            if lo_t is not None:
                l_tw = lo_t[0]; l_th = lo_t[1]; l_grid = lo_t[2]
            # Where the falloff is saturated the shade is a function of the
            # sector light and the fake-contrast weight alone - both per-seg -
            # so both ends fold to a constant and the saturated columns, which
            # are most of them, cost an assignment instead of a divide, a
            # truncation, a multiply and a floor-divide.
            shw_far = (int(lf * 0.68 * 256) * wk) // 100
            if shw_far > 256: shw_far = 256
            shw_near = (int(lf * 1.0 * 256) * wk) // 100
            if shw_near > 256: shw_near = 256
            # Flat-fallback colours depend only on the sector light.
            ceil_fb = None if cgrid is not None else col_rgb(C_CEIL, lf * 0.9)
            floor_fb = None if fgrid is not None else col_rgb(C_FLOOR, lf * 0.9)
            # Masked midtexture: the top plane is a property of the seg, not
            # of the column.  a * b * c already groups as (a * b) * c, so
            # lifting (zt - viewz) * vsc out is exact, not an approximation.
            # Texture coordinates are only needed if this seg actually has a
            # textured piece.  A plain portal - an open doorway with no upper,
            # lower or midtexture - emits nothing but flats, and computing u,
            # dv and their fixed-point forms for it was pure waste.
            if twosided:
                need_uv = (mid_t is not None
                           or (bc < fc and not bothsky and up_t is not None)
                           or (bf > ff and lo_t is not None))
            else:
                need_uv = mid_t is not None
            kzt = 0.0
            if twosided and mid_t is not None:
                if flags & ML_DONTPEGBOT:
                    zt = (ff if ff > bf else bf) + th_m
                else:
                    zt = fc if fc < bc else bc
                kzt = (zt - viewz) * vsc
            closed = 0
            for x in range(ix1, ix2 + 1):
                ct = _ceilc[x]; cb = _floorc[x]
                if ct > cb:
                    continue
                xf = x - rx1
                invd = inv1 + k_invd * xf
                dist = 1.0 / invd
                # df saturates outside [DF_NEAR, DF_FAR], and most columns
                # of most frames are outside it, so test the distance first
                # and skip the divide.  Same values as clamping afterwards:
                # at either bound the quotient is exactly the clamp.
                if dist > DF_FAR:
                    df = 0.68; shw = shw_far
                elif dist < DF_NEAR:
                    df = 1.0; shw = shw_near
                else:
                    df = 1200.0 / (dist + 650.0)
                    shw = (int(lf * df * 256) * wk) // 100
                    if shw > 256: shw = 256
                if need_uv:
                    u = (uz1 + k_uz * xf) * dist
                    dv = dist * inv_vsc
                    dvfp = int(dv * 256)
                    iu = int(u)
                yfc = int(hh - kfc * invd)
                yff = int(hh - kff * invd)
                x0 = _colx[x]; xw = _colw[x]
                if twosided:
                    ybc = int(hh - kbc * invd)
                    ybf = int(hh - kbf * invd)
                    if bothsky and ybc > yfc:
                        yfc = ybc
                # ---- ceiling (or sky) above the front ceiling
                if yfc > ct:
                    y1 = (yfc - 1) if (yfc - 1) < cb else cb
                    y0 = ct
                    if y1 >= y0:
                        if y0 < 0: y0 = 0
                        if y1 > chm1: y1 = chm1
                        if y1 >= y0:
                            if csky:
                                if sky_t is None:
                                    _append(('R', x0, y0, xw, y1 - y0 + 1,
                                             sky_rgb))
                                else:
                                    _append(('W', x0, xw, y0, y1 - y0 + 1,
                                             sky_t[2], sky_t[0], sky_t[1],
                                             _skyu[x], y0 * dvsky, dvsky, 256))
                            elif cgrid is not None:
                                _append(('F', x0, xw, y0, y1 - y0 + 1, cgrid,
                                         kfc, _dcx[x], _dcy[x], lf))
                            else:
                                _append(('R', x0, y0, xw, y1 - y0 + 1, ceil_fb))
                # ---- floor below the front floor
                if yff < cb:
                    y0 = (yff + 1) if (yff + 1) > ct else ct
                    y1 = cb
                    if y1 >= y0:
                        if y0 < 0: y0 = 0
                        if y1 > chm1: y1 = chm1
                        if y1 >= y0:
                            if fgrid is not None:
                                _append(('F', x0, xw, y0, y1 - y0 + 1, fgrid,
                                         kff, _dcx[x], _dcy[x], lf))
                            else:
                                _append(('R', x0, y0, xw, y1 - y0 + 1, floor_fb))
                if not twosided:
                    # ---- solid wall: closes the column for good
                    y0 = yfc if yfc > ct else ct
                    y1 = yff if yff < cb else cb
                    if y0 < 0: y0 = 0
                    if y1 > chm1: y1 = chm1
                    if y1 >= y0:
                        if mid_t is None:
                            _append(('R', x0, y0, xw, y1 - y0 + 1,
                                     rgb((200 * shw) >> 8, (188 * shw) >> 8,
                                         (170 * shw) >> 8)))
                        else:
                            _append(('W', x0, xw, y0, y1 - y0 + 1, m_grid,
                                     m_tw, m_th, iu,
                                     int((v_mid + (y0 - yfc) * dv) * 256),
                                     dvfp, shw))
                    _ceilc[x] = 1; _floorc[x] = 0
                    closed += 1
                    _clips[x].append((dist, 1, 0))
                else:
                    newct = ct if ct > yfc else yfc
                    newcb = cb if cb < yff else yff
                    # ---- two-sided midtexture (a grate, a bar): goes to the
                    # depth-sorted masked pass, not straight out
                    if mid_t is not None:
                        yot = yfc if yfc > ybc else ybc
                        yob = yff if yff < ybf else ybf
                        ytt = hh - kzt * invd
                        my0 = int(ytt)
                        my1 = int(ytt + th_m / dv)
                        if my0 < yot: my0 = yot
                        if my0 < ct: my0 = ct
                        if my0 < 0: my0 = 0
                        if my1 > yob: my1 = yob
                        if my1 > cb: my1 = cb
                        if my1 > chm1: my1 = chm1
                        if my1 >= my0:
                            _masked.append((dist, ('M', x0, xw, my0,
                                            my1 - my0 + 1, m_grid, m_tw, th_m,
                                            iu,
                                            int((yoff + (my0 - ytt) * dv) * 256),
                                            dvfp, shw)))
                    # ---- upper piece, where the back ceiling is lower
                    if bc < fc and not bothsky:
                        if up_t is not None:
                            y0 = yfc if yfc > ct else ct
                            y1 = (ybc - 1) if (ybc - 1) < newcb else newcb
                            if y0 < 0: y0 = 0
                            if y1 > chm1: y1 = chm1
                            if y1 >= y0:
                                _append(('W', x0, xw, y0, y1 - y0 + 1, u_grid,
                                         u_tw, u_th, iu,
                                         int((v_up + (y0 - yfc) * dv) * 256),
                                         dvfp, shw))
                        else:
                            y0 = yfc if yfc > ct else ct
                            y1 = (ybc - 1) if (ybc - 1) < newcb else newcb
                            if y1 >= y0:
                                if y0 < 0: y0 = 0
                                if y1 > chm1: y1 = chm1
                                if y1 >= y0:
                                    _append(('R', x0, y0, xw, y1 - y0 + 1,
                                             col_rgb(C_CEIL, lf * df)))
                        if ybc > newct: newct = ybc
                    # ---- lower piece, where the back floor is higher
                    if bf > ff:
                        if lo_t is not None:
                            ybf1 = ybf + 1
                            y0 = ybf1 if ybf1 > newct else newct
                            y1 = yff if yff < cb else cb
                            if y0 < 0: y0 = 0
                            if y1 > chm1: y1 = chm1
                            if y1 >= y0:
                                _append(('W', x0, xw, y0, y1 - y0 + 1, l_grid,
                                         l_tw, l_th, iu,
                                         int((v_lo + (y0 - ybf1) * dv) * 256),
                                         dvfp, shw))
                        else:
                            y0 = (ybf + 1) if (ybf + 1) > newct else newct
                            y1 = yff if yff < cb else cb
                            if y1 >= y0:
                                if y0 < 0: y0 = 0
                                if y1 > chm1: y1 = chm1
                                if y1 >= y0:
                                    _append(('R', x0, y0, xw, y1 - y0 + 1,
                                             col_rgb(C_FLOOR, lf * df)))
                        if ybf < newcb: newcb = ybf
                    _ceilc[x] = newct; _floorc[x] = newcb
                    if newct > newcb:
                        closed += 1
                    if newct > ct or newcb < cb:
                        _clips[x].append((dist, newct, newcb))
            state[0] -= closed

        def bbox_vis(top, bot, left, right):
            # Conservative frustum test: skip the far child only when all 4
            # corners are behind the near plane or project past one screen
            # edge, or every covered column is already closed.
            #
            # Unrolled over the four corners.  This runs for every node of
            # the BSP, and the loop it replaces built a tuple of four tuples
            # on each call, then recomputed both products per corner - when
            # the four corners between them have only two distinct dx and
            # two distinct dy, so eight products cover all sixteen terms.
            dxl = left - px; dxr = right - px
            dyt = top - py; dyb = bot - py
            lc = dxl * cos_a; rc = dxr * cos_a
            ts = dyt * sin_a; bs_ = dyb * sin_a
            ls = dxl * sin_a; rs = dxr * sin_a
            tc = dyt * cos_a; bc_ = dyb * cos_a
            all_behind = True
            lo = 1e9; hi = -1e9
            straddle = False
            depth = lc + ts
            if depth >= NEAR:
                all_behind = False
                lo = hi = hwf + ((ls - tc) / depth) * scale
            else:
                straddle = True
            depth = rc + ts
            if depth >= NEAR:
                all_behind = False
                r = hwf + ((rs - tc) / depth) * scale
                if r < lo: lo = r
                if r > hi: hi = r
            else:
                straddle = True
            depth = lc + bs_
            if depth >= NEAR:
                all_behind = False
                r = hwf + ((ls - bc_) / depth) * scale
                if r < lo: lo = r
                if r > hi: hi = r
            else:
                straddle = True
            depth = rc + bs_
            if depth >= NEAR:
                all_behind = False
                r = hwf + ((rs - bc_) / depth) * scale
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
                    ssi = n & 0x7FFF
                    ss = m.ssectors[ssi]
                    seen[sssec[ssi]] = stamp     # this sector can show things
                    first = ss[1]
                    for i in range(first, first + ss[0]):
                        draw_seg(i)
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
        # Reject a thing on two multiplies if it cannot put a pixel on
        # screen.  The old test was `depth >= NEAR` alone, so everything in
        # front of the viewer - including the whole level off to either side
        # - paid for a sprite lookup, an atan2 and a full projection before
        # the column clamps quietly threw the result away.  SPR_HW is a
        # world-space half-width larger than any sprite's extent, which makes
        # the cull conservative: it can only drop things whose projection
        # would have clamped to an empty column range anyway.
        vis = []
        tanh = (RW / 2) / scale          # half-width of the frustum per unit depth
        for t in self.things_live:
            if seen[t[14]] != stamp:     # sector the walk never reached
                continue
            dxt = t[0] - px; dyt = t[1] - py
            depth = dxt * cos_a + dyt * sin_a
            if depth < NEAR:
                continue
            lim = depth * tanh + SPR_HW
            sxt = dxt * sin_a - dyt * cos_a
            if sxt < -lim or sxt > lim:
                continue
            vis.append((depth, 1, t, dxt, dyt))
        for f in self.fx:
            dxt = f[0] - px; dyt = f[1] - py
            depth = dxt * cos_a + dyt * sin_a
            if depth >= NEAR:
                lim = depth * tanh + SPR_HW
                sxt = dxt * sin_a - dyt * cos_a
                if -lim <= sxt <= lim:
                    vis.append((depth, 2, f, dxt, dyt))
        for p in self.proj:
            dxt = p[0] - px; dyt = p[1] - py
            depth = dxt * cos_a + dyt * sin_a
            if depth >= NEAR:
                lim = depth * tanh + SPR_HW
                sxt = dxt * sin_a - dyt * cos_a
                if -lim <= sxt <= lim:
                    vis.append((depth, 3, p, dxt, dyt))
        for (d, op) in masked:
            vis.append((d, 0, op, 0.0, 0.0))
        vis.sort(key=lambda e: -e[0])
        spr = self.spr
        A202 = 3.5342917                   # 202.5 degrees
        animclock = int(self.now * 5.0)
        for (depth, kind, t, dxt, dyt) in vis:
            if kind == 0:
                out.append(t)              # pre-built masked midtex column
                continue
            fullbright = 0
            zoff = None
            if kind == 1:
                frames = t[5]
                st = t[10]
                if st == 0:
                    if t[9] == 0 and t[3] != 2035:
                        fi = animclock % len(frames)       # decor/pickup cycle
                    else:
                        fi = (animclock >> 2) % len(frames)  # idle monster
                else:
                    fi = t[8]
                    if fi >= len(frames): fi = len(frames) - 1
                fch = frames[fi:fi + 1]
                # Doom: rot = (viewer->thing angle) - thing facing + 202.5 deg
                rel = (math.atan2(dyt, dxt) - t[2] + A202) % 6.2831853
                s = spr.get(t[4], fch, int(rel / 0.7853982) & 7)
            elif kind == 2:                # effect: explicit z, fullbright
                fi = int(t[5] * t[6])
                if fi >= len(t[4]): fi = len(t[4]) - 1
                s = spr.get(t[3], t[4][fi:fi + 1], 0)
                fullbright = 1
                zoff = t[2]
            else:                          # projectile in flight
                base = t[5]
                fr = b"A" if base == b"MISL" else b"AB"
                s = spr.get(base, fr[int(t[6] * 8) % len(fr):][:1], 0)
                fullbright = 1
                zoff = t[2]
            if s is None:
                continue
            w, h, lo, to, grid, mirror = s
            invd = 1.0 / depth
            scr = scale * invd             # screen cols per world unit
            sxc = dxt * sin_a - dyt * cos_a
            rxl = (RW / 2) + (sxc - lo) * scr
            du = depth / scale
            if kind == 1:
                sec = t[7]
                ztop = sec[1] if t[6] else (sec[0] + to)
                light = sec[4]
            else:
                ztop = zoff + to
                light = 255
            ytt = hh - (ztop - viewz) * vsc * invd
            dv = depth / vsc
            iy0 = int(ytt); iy1 = int(ytt + h / dv) - 1
            if iy1 < 0 or iy0 > ch - 1:
                continue
            ix0 = int(rxl); ix1 = int(rxl + w * scr)
            if ix0 < 0: ix0 = 0
            if ix1 > RW - 1: ix1 = RW - 1
            if fullbright:
                sh = 256
            else:
                df = 1200.0 / (depth + 650.0)
                if df > 1.0: df = 1.0
                elif df < 0.68: df = 0.68
                sh = int((0.72 + 0.28 * (light / 255.0)) * df * 256)
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
                x0s = colx[x]; x1s = colx[x + 1]
                out.append(('M', x0s, x1s - x0s, y0, y1 - y0 + 1, grid, w, h,
                            texcol, int((y0 - ytt) * dv * 256), dvfp, sh))

        # ---- player weapon sprite (320x200 screen space, scaled) -----------
        if not self.dead:
            psc = cw / 320.0
            ysc = ch / 200.0
            dvp = int(256.0 / ysc)
            bobx = math.sin(self.bob) * 6.0 * self.bobamp
            boby = abs(math.sin(self.bob * 2.0)) * 4.0 * self.bobamp

            def psop(pic, sh):
                w_, h_, lo_, to_, grid_ = pic
                xl = (1 - lo_) + bobx
                yt = (32 + boby) - to_
                y0 = int(yt * ysc)
                if y0 < 0: y0 = 0
                cnt = ch - y0
                if cnt > int(h_ * ysc): cnt = int(h_ * ysc)
                if cnt <= 0: return
                for c in range(w_):
                    xs = int((xl + c) * psc)
                    xe = int((xl + c + 1) * psc)
                    if xe > cw: xe = cw
                    if xs < 0 or xs >= xe: continue
                    out.append(('M', xs, xe - xs, y0, cnt, grid_, w_, h_,
                                c, 0, dvp, sh))

            wdef = WEAP[self.weapon]
            firing = self.now < self.wflash_until
            wl = self.lvl.sectors[self.psec_i][4]
            shp = int((0.72 + 0.28 * (wl / 255.0)) * 256)
            gun = spr.ui(wdef[1] + (b"B0" if firing else b"A0"))
            if gun is None:
                gun = spr.ui(wdef[1] + b"A0")
            if gun:
                psop(gun, shp)
            if firing and wdef[2]:
                fl = spr.ui(wdef[2] + b"A0")
                if fl:
                    psop(fl, 256)

        # ---- status bar (rebuilt only when its inputs change) --------------
        wk = WEAP[self.weapon][4]
        hud_state = (self.health, self.armor, self.weapon, self.keys,
                     tuple(self.ammo), tuple(self.maxammo),
                     tuple(self.owned), self.dead)
        if hud_state != self.hud_key:
            self.hud_key = hud_state
            self.hud_ops = self.build_hud()
        out.extend(self.hud_ops)
        self.frame = out

    def build_hud(self):
        """The status bar as a list of masked ops in canvas space."""
        cw = self.cw; ch = self.ch
        s = cw / 320.0
        ybar = ch - int(32 * s)
        dvp = int(256.0 / s)
        ops = []
        ui = self.spr.ui

        def pic(name, x320, y32):
            p = ui(name)
            if p is None:
                return
            w, h, lo, to, grid = p
            y0 = ybar + int(y32 * s)
            cnt = int(h * s + 0.999)
            if y0 + cnt > ch:
                cnt = ch - y0
            if cnt <= 0:
                return
            for c in range(w):
                xs = int((x320 + c) * s)
                xe = int((x320 + c + 1) * s)
                if xe > cw: xe = cw
                if xs >= xe: continue
                ops.append(('M', xs, xe - xs, y0, cnt, grid, w, h, c, 0,
                            dvp, 256))

        def num(n, xr, y32, yellow=0, gray=0):
            x = xr
            for chd in reversed(str(n)):
                nm = (b"STYSNUM" if yellow else b"STGNUM" if gray
                      else b"STTNUM") + chd.encode()
                p = ui(nm)
                if p is None:
                    continue
                x -= p[0]
                pic(nm, x, y32)

        pic(b"STBAR", 0, 0)
        pic(b"STARMS", 104, 0)
        wk = WEAP[self.weapon][4]
        if wk:
            num(self.ammo[wk], 44, 3)
        num(self.health, 84, 3)
        pic(b"STTPRCNT", 90, 3)
        num(self.armor, 215, 3)
        pic(b"STTPRCNT", 221, 3)
        if self.dead:
            pic(b"STFDEAD0", 143, 0)
        else:
            pain = (100 - self.health) * 5 // 101
            if pain > 4: pain = 4
            pic(b"STFST" + str(pain).encode() + b"1", 143, 0)
        if self.keys & 1: pic(b"STKEYS0", 239, 3)
        if self.keys & 4: pic(b"STKEYS1", 239, 13)
        if self.keys & 2: pic(b"STKEYS2", 239, 23)
        arms = ((2, 111, 4), (3, 119, 4), (4, 127, 4), (5, 111, 14),
                (6, 119, 14))
        for (wpn, x, y) in arms:
            nm = (b"STYSNUM" if wpn in self.owned else b"STGNUM") + \
                str(wpn).encode()
            pic(nm, x, y)
        rows = ((1, 5), (2, 11), (3, 17), (4, 23))
        for (k, y) in rows:
            num(self.ammo[k], 288, y, yellow=1)
            num(self.maxammo[k], 314, y, yellow=1)
        return ops

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
            if k == 'W':
                cv.wall_span(op[1], op[2], op[3], op[4], op[5], op[6], op[7],
                             op[8], op[9], op[10], pal, op[11])
            elif k == 'M':
                cv.mask_span(op[1], op[2], op[3], op[4], op[5], op[6], op[7],
                             op[8], op[9], op[10], pal, op[11])
            elif k == 'F':
                cv.flat_span(op[1], op[2], op[3], op[4], op[5], pal, op[6],
                             hh, op[7], op[8], vx, vy, op[9])
            else:
                cv.fill_rect(op[1], op[2], op[3], op[4], op[5])
        if self.msg:
            cv.text(6, 6, self.msg, rgb(235, 235, 210))
        if self.finish is not None:
            cx = self.cw // 2
            cy = self.ch // 2
            cv.fill_rect(cx - 130, cy - 34, 260, 64, rgb(16, 16, 20))
            cv.text(cx - 60, cy - 24, self.level + " FINISHED", rgb(250, 60, 60))
            nxt = self.finish[0]
            if nxt is None:
                cv.text(cx - 100, cy - 4, "KNEE-DEEP IN THE DEAD complete!",
                        rgb(235, 235, 210))
            else:
                cv.text(cx - 60, cy - 4, "Entering " + nxt, rgb(235, 235, 210))
            cv.text(cx - 76, cy + 14, "press SPACE to continue", rgb(180, 180, 180))

    # ---- input --------------------------------------------------------------
    # There are no key-up events on this platform.  A key event marks the key
    # held for the next 0.3s and typematic repeats keep refreshing it; when a
    # real uno.keys_down() exists it overrides this timer model.
    def key(self, uni, scan, ctrl):
        if self.err is not None:
            return False
        if self.dead or self.finish is not None:
            if uni == 32 or scan == 0x01 or uni == 13:
                if self.finish is not None:
                    nxt = self.finish[0]
                    if nxt is None:
                        self.new_game("E1M1")
                    else:
                        self.load_level(nxt)
                else:
                    self.new_game(self.level)      # died: restart the level
            return True
        exp = self.now + 0.30
        if scan in (1, 2, 3, 4):
            self.held[scan] = exp
            return True
        u = uni | 32 if 65 <= uni <= 90 else uni
        if u == 44 or u == 46:
            self.held[u] = exp
            return True
        if u == 102 or ctrl:                       # f (or Ctrl chord) = fire
            self.held[102] = exp
            return True
        if u == 32 or u == 101:                    # Space/e = use
            self.use_press = True
            return True
        if 49 <= u <= 54:                          # 1..6 select weapon
            w = u - 48
            if w in self.owned:
                if self.ammo[WEAP[w][4]] >= WEAP[w][5] or WEAP[w][4] == 0:
                    self.weapon = w
                    self.msgset(WEAP[w][0].decode() + ".")
            return True
        return False

    def msgset(self, s):
        self.msg = s
        self.msg_until = self.now + 3.0

    # key -> UNO_KH_* bit for the live held-state export (uno.keys_down)
    KDBITS = {1: 1, 2: 2, 3: 4, 4: 8, 102: 16, 44: 64, 46: 128}

    def _held(self, k):
        b = Duum.KDBITS.get(k, 0)
        if self.kd & b:
            return True
        e = self.held.get(k)
        return e is not None and e > self.now

    # ---- the game loop ------------------------------------------------------
    def tick(self):
        if self.err is not None:
            return False
        if self.have_ticks:
            tk = uno.ticks()
            if self.last_ticks is None:
                self.last_ticks = tk
            dt = (tk - self.last_ticks) / 60.0
            self.last_ticks = tk
            if dt <= 0:
                dt = 0.008
        else:
            dt = 0.033
        if dt > 0.1:
            dt = 0.1
        self.now += dt
        if self.have_keys:
            self.kd_prev = self.kd
            self.kd = uno.keys_down()
            if (self.kd & 32) and not (self.kd_prev & 32):
                self.use_press = True          # rising edge of Space/E
        moved = False
        if not self.dead and self.finish is None:
            moved = self.step_player(dt)
            if self.use_press:
                self.use_press = False
                self.do_use()
            if self._held(102):
                self.try_fire()
            self.floor_damage()
        self.run_movers(dt)
        self.run_monsters(dt)
        self.run_proj(dt)
        self.run_fx(dt)
        if self.msg and self.now > self.msg_until:
            self.msg = ""
        # Render every tick while something is visibly moving; fall back to
        # the ~6Hz ambient-animation rate when the world is quiet, and let
        # the shell idle in between (tr_frame repaints only on True).
        act = (moved or self.dirty or self.movers or self.proj or self.fx
               or self.mon_active or self.now < self.wflash_until + 0.2)
        if not act and self.now < self.anim_next:
            return False
        self.anim_next = self.now + 0.16
        self.dirty = False
        self.render()
        return True

    def step_player(self, dt):
        ca = math.cos(self.pa); sa = math.sin(self.pa)
        moved = False
        if self._held(4):
            self.pa += TURNSPD * dt; moved = True
        if self._held(3):
            self.pa -= TURNSPD * dt; moved = True
        vx = vy = 0.0
        if self._held(1): vx += ca; vy += sa
        if self._held(2): vx -= ca; vy -= sa
        if self._held(44): vx -= sa; vy += ca
        if self._held(46): vx += sa; vy -= ca
        if vx != 0.0 or vy != 0.0:
            mv = MOVSPD * dt
            ox, oy = self.px, self.py
            nx = ox + vx * mv; ny = oy + vy * mv
            if not self.blocked(ox, oy, nx, ny, 16, None):
                self.px = nx; self.py = ny
            elif not self.blocked(ox, oy, nx, oy, 16, None):
                self.px = nx
            elif not self.blocked(ox, oy, ox, ny, 16, None):
                self.py = ny
            if self.px != ox or self.py != oy:
                moved = True
                self.bob += dt * 10.0
                self.bobamp = 1.0
                self.psec_i = self.point_secidx(self.px, self.py)
                self.check_cross(ox, oy)
                self.check_pickups()
        else:
            self.bobamp *= (1.0 - dt * 6.0)
            if self.bobamp < 0.0:
                self.bobamp = 0.0
        return moved

    def blocked(self, ox, oy, nx, ny, r, selfthing):
        s = self.point_sector(nx, ny)
        if s is None:
            return True
        cur = self.point_sector(ox, oy)
        if cur is not None and s[0] - cur[0] > 24:     # max step height
            return True
        if s[1] - s[0] < 56:                           # headroom
            return True
        for t in self.things_live:
            rr = t[12]
            if rr == 0 or t is selfthing:
                continue
            if abs(t[0] - nx) < rr + r and abs(t[1] - ny) < rr + r:
                if abs(t[0] - ox) < rr + r and abs(t[1] - oy) < rr + r:
                    continue                           # already overlapping
                return True
        if selfthing is not None:                      # monsters hit the player
            if abs(self.px - nx) < 16 + r and abs(self.py - ny) < 16 + r:
                return True
        return False

    # ---- lines: use, walkover triggers, tagged actions ----------------------
    def do_use(self):
        ux = math.cos(self.pa); uy = math.sin(self.pa)
        m = self.lvl
        best = None; bestt = 65.0
        for li in range(len(m.lines)):
            ld = m.lines[li]
            if ld[3] == 0:
                continue
            x1, y1 = m.verts[ld[0]]; x2, y2 = m.verts[ld[1]]
            ex = x2 - x1; ey = y2 - y1
            den = ux * ey - uy * ex
            if den == 0:
                continue
            tt = ((x1 - self.px) * ey - (y1 - self.py) * ex) / den
            if tt <= 0 or tt >= bestt:
                continue
            if abs(ex) >= abs(ey):
                s = (self.px + tt * ux - x1) / ex
            else:
                s = (self.py + tt * uy - y1) / ey
            if 0.0 <= s <= 1.0:
                best = li; bestt = tt
        if best is not None:
            self.activate(best, 1)
        else:
            self.snd(40, 3)

    def activate(self, li, by_use):
        m = self.lvl
        ld = m.lines[li]
        sp = ld[3]
        d = DOOR_USE.get(sp)
        if d is not None:
            if not by_use or ld[6] == 0xFFFF:
                return
            need, stay = d
            if (self.keys & need) != need:
                self.msgset("You need a %s key." %
                            ("blue" if need == 1 else
                             "red" if need == 2 else "yellow"))
                self.snd(45, 4)
                return
            si = m.sides[ld[6]][5]
            self.start_door(si, 'open' if stay else 'door')
            if stay:
                m.lines[li][3] = 0
            return
        a = ACT.get(sp)
        if a is None:
            return
        kind, walk = a
        if by_use and walk and kind != 4 and kind != 5:
            return
        if not by_use and not walk:
            return
        tag = ld[4]
        if kind == 4 or kind == 5:
            self.start_exit(kind == 5)
        elif kind == 6:
            self.teleport(tag)
        else:
            for si in m.tagsec.get(tag, ()):
                if kind == 0:
                    self.start_door(si, 'door')
                elif kind == 1:
                    self.start_door(si, 'open')
                elif kind == 7:
                    self.start_door(si, 'close')
                elif kind == 2:
                    self.start_lift(si)
                elif kind == 3:
                    self.start_floor_lower(si)
        if sp in ONE_SHOT:
            m.lines[li][3] = 0
        if by_use:
            self.flip_switch(li)

    def flip_switch(self, li):
        m = self.lvl
        ld = m.lines[li]
        if ld[5] == 0xFFFF:
            return
        sd = m.sides[ld[5]]
        for f in (2, 3, 4):
            n = sd[f]
            if n[:3] == b"SW1":
                sd[f] = b"SW2" + n[3:]
            elif n[:3] == b"SW2":
                sd[f] = b"SW1" + n[3:]
        self.segrec = None            # cached texture tuples are now stale
        self.snd(50, 3)

    def _sector_busy(self, si):
        for mv in self.movers:
            if mv.si == si:
                return True
        return False

    def start_door(self, si, kind):
        if self._sector_busy(si):
            return
        m = self.lvl
        sec = m.sectors[si]
        if kind == 'close':
            target = sec[0]
        else:
            target = 32767
            for ni in m.adj[si]:
                c = m.sectors[ni][1]
                if c < target:
                    target = c
            target -= 4
            if target <= sec[1] and kind != 'close':
                target = sec[1] + 1        # already open-ish; nudge
        self.movers.append(Mover(kind, si, sec, target))
        self.snd(28, 6)

    def start_lift(self, si):
        if self._sector_busy(si):
            return
        m = self.lvl
        sec = m.sectors[si]
        low = sec[0]
        for ni in m.adj[si]:
            f = m.sectors[ni][0]
            if f < low:
                low = f
        self.movers.append(Mover('lift', si, sec, low))
        self.snd(24, 8)

    def start_floor_lower(self, si):
        if self._sector_busy(si):
            return
        m = self.lvl
        sec = m.sectors[si]
        low = sec[0]
        for ni in m.adj[si]:
            f = m.sectors[ni][0]
            if f < low:
                low = f
        self.movers.append(Mover('floor', si, sec, low))
        self.snd(24, 8)

    def start_exit(self, secret):
        if secret and self.level in SECRET_MAP:
            nxt = SECRET_MAP[self.level]
        elif self.level in AFTER_SECRET:
            nxt = AFTER_SECRET[self.level]
        elif self.level == "E1M8":
            nxt = None                     # episode complete
        else:
            nxt = EP_ORDER[EP_ORDER.index(self.level) + 1]
        self.finish = (nxt, self.now)
        self.snd(60, 8)

    def teleport(self, tag):
        m = self.lvl
        dests = m.tagsec.get(tag, ())
        for (x, y, ang, typ, fl) in m.things:
            if typ == 14 and self.point_secidx(x, y) in dests:
                self.fx.append([self.px, self.py,
                                self.point_sector(self.px, self.py)[0],
                                b"TFOG", b"ABABCDEFGHIJ", 0.0, 15])
                self.px = float(x); self.py = float(y)
                self.pa = math.radians(ang)
                self.psec_i = self.point_secidx(x, y)
                self.fx.append([x, y, self.lvl.sectors[self.psec_i][0],
                                b"TFOG", b"ABABCDEFGHIJ", 0.0, 15])
                self.snd(70, 6)
                return

    def check_cross(self, ox, oy):
        m = self.lvl
        nx, ny = self.px, self.py
        for li in m.trig:
            ld = m.lines[li]
            if ld[3] == 0:
                continue
            x1, y1 = m.verts[ld[0]]; x2, y2 = m.verts[ld[1]]
            ex = x2 - x1; ey = y2 - y1
            s1 = (ox - x1) * ey - (oy - y1) * ex
            s2 = (nx - x1) * ey - (ny - y1) * ex
            if (s1 > 0) == (s2 > 0):
                continue
            dx = nx - ox; dy = ny - oy
            den = dx * ey - dy * ex
            if den == 0:
                continue
            tt = ((x1 - ox) * ey - (y1 - oy) * ex) / den
            if tt < 0.0 or tt > 1.0:
                continue
            if abs(ex) >= abs(ey):
                s = (ox + tt * dx - x1) / ex
            else:
                s = (oy + tt * dy - y1) / ey
            if 0.0 <= s <= 1.0:
                self.activate(li, 0)

    # ---- pickups ------------------------------------------------------------
    def check_pickups(self):
        px, py = self.px, self.py
        got = None
        for t in self.things_live:
            p = PICKUP.get(t[3])
            if p is None or t[10] != 0:
                continue
            if abs(t[0] - px) < 40 and abs(t[1] - py) < 40:
                if self.apply_pickup(t[3], p):
                    got = t
                    break
        if got is not None:
            self.things_live.remove(got)
            self.snd(80, 2)
            self.dirty = True

    def apply_pickup(self, typ, p):
        kind, a, b, msg = p
        took = True
        if kind == 0:
            if self.health >= b and a > 1:
                return False
            self.health = min(self.health + a, b)
            if typ == 2023:
                self.berserk = True
        elif kind == 1:
            if self.armor >= a:
                return False
            self.armor = a
        elif kind == 2:
            if self.armor >= 200:
                return False
            self.armor += 1
        elif kind == 3:
            if self.ammo[a] >= self.maxammo[a]:
                return False
            self.ammo[a] = min(self.ammo[a] + b, self.maxammo[a])
        elif kind == 4:
            w = WEAP[a]
            self.owned[a] = 1
            self.ammo[w[4]] = min(self.ammo[w[4]] + b * 2, self.maxammo[w[4]])
            self.weapon = a
        elif kind == 5:
            self.keys |= a
        elif kind == 6:
            for i in range(1, 5):
                self.maxammo[i] = AMMOMAX[i] * 2
                self.ammo[i] = min(self.ammo[i] + (10, 4, 1, 20)[i - 1],
                                   self.maxammo[i])
        elif kind == 7:
            if typ == 2022:
                self.invuln_until = self.now + 30.0
            elif typ == 2025:
                self.suit_until = self.now + 60.0
        self.msgset(msg)
        self.hud_key = None
        return took

    # ---- combat -------------------------------------------------------------
    def trace_wall(self, x, y, dx, dy, maxd):
        """Nearest bullet-blocking wall crossing along a ray -> distance."""
        m = self.lvl
        verts = m.verts; sides = m.sides; sectors = m.sectors
        best = maxd
        for ld in m.lines:
            x1, y1 = verts[ld[0]]; x2, y2 = verts[ld[1]]
            ex = x2 - x1; ey = y2 - y1
            den = dx * ey - dy * ex
            if den == 0:
                continue
            tt = ((x1 - x) * ey - (y1 - y) * ex) / den
            if tt <= 0 or tt >= best:
                continue
            if abs(ex) >= abs(ey):
                s = (x + tt * dx - x1) / ex
            else:
                s = (y + tt * dy - y1) / ey
            if s < 0.0 or s > 1.0:
                continue
            if ld[6] == 0xFFFF:
                best = tt
                continue
            fs = sectors[sides[ld[5]][5]]
            bs = sectors[sides[ld[6]][5]]
            top = fs[1] if fs[1] < bs[1] else bs[1]
            bot = fs[0] if fs[0] > bs[0] else bs[0]
            if top - bot < 12:
                best = tt
        return best

    def sight_clear(self, x1, y1, x2, y2):
        """Line of sight between two points (2D + portal opening check)."""
        m = self.lvl
        verts = m.verts; sides = m.sides; sectors = m.sectors
        dx = x2 - x1; dy = y2 - y1
        for ld in m.lines:
            ax, ay = verts[ld[0]]; bx, by = verts[ld[1]]
            ex = bx - ax; ey = by - ay
            s1 = (x1 - ax) * ey - (y1 - ay) * ex
            s2 = (x2 - ax) * ey - (y2 - ay) * ex
            if (s1 > 0) == (s2 > 0):
                continue
            t1 = (ax - x1) * dy - (ay - y1) * dx
            t2 = (bx - x1) * dy - (by - y1) * dx
            if (t1 > 0) == (t2 > 0):
                continue
            if ld[6] == 0xFFFF:
                return False
            fs = sectors[sides[ld[5]][5]]
            bs = sectors[sides[ld[6]][5]]
            top = fs[1] if fs[1] < bs[1] else bs[1]
            bot = fs[0] if fs[0] > bs[0] else bs[0]
            if top - bot < 16:
                return False
        return True

    def try_fire(self):
        if self.now < self.refire_at:
            return
        w = WEAP[self.weapon]
        ak = w[4]
        if ak and self.ammo[ak] < w[5]:
            self.snd(35, 2)
            if self.weapon > 2:            # dry: drop to pistol or fist
                self.weapon = 2 if self.ammo[1] > 0 else 1
                self.hud_key = None
            return
        if ak:
            self.ammo[ak] -= w[5]
            self.hud_key = None
        self.refire_at = self.now + w[3]
        self.wflash_until = self.now + 0.12
        self.noise_wake()
        wk = self.weapon
        if wk == 1:
            self.snd(30, 2)
            self.melee_attack()
        elif wk == 2:
            self.snd(55, 2)
            self.hitscan(self.pa, 5 + self.rnd() % 11)
        elif wk == 3:
            self.snd(48, 3)
            for _ in range(7):
                sp = (self.rnd() - 128) / 1300.0
                self.hitscan(self.pa + sp, 5 + self.rnd() % 11)
        elif wk == 4:
            self.snd(55, 1)
            sp = (self.rnd() - 128) / 2200.0
            self.hitscan(self.pa + sp, 5 + self.rnd() % 11)
        elif wk == 5:
            self.snd(38, 4)
            self.spawn_proj(0, b"MISL", 20)
        elif wk == 6:
            self.snd(66, 1)
            self.spawn_proj(0, b"PLSS", 5)

    def melee_attack(self):
        ca = math.cos(self.pa); sa = math.sin(self.pa)
        best = None; bestd = 80.0
        for t in self.things_live:
            if t[9] <= 0 or t[10] >= 4:
                continue
            dx = t[0] - self.px; dy = t[1] - self.py
            d = dx * ca + dy * sa
            if d <= 0 or d >= bestd:
                continue
            if abs(dx * sa - dy * ca) < t[12] + 20:
                best = t; bestd = d
        if best is not None:
            dm = (1 + self.rnd() % 10) * 2
            if self.berserk:
                dm *= 10
            self.damage_monster(best, dm)

    def hitscan(self, ang, dmg):
        ca = math.cos(ang); sa = math.sin(ang)
        wd = self.trace_wall(self.px, self.py, ca, sa, 3000.0)
        best = None; bestd = wd
        for t in self.things_live:
            if t[9] <= 0 or t[10] >= 4:
                continue
            dx = t[0] - self.px; dy = t[1] - self.py
            d = dx * ca + dy * sa
            if d <= 0 or d >= bestd:
                continue
            if abs(dx * sa - dy * ca) < t[12] + 6:
                best = t; bestd = d
        if best is not None:
            self.damage_monster(best, dmg)
            self.fx.append([best[0], best[1], best[7][0] + 32, b"BLUD",
                            b"CBA", 0.0, 12])
        else:
            hx = self.px + ca * (wd - 8)
            hy = self.py + sa * (wd - 8)
            s = self.point_sector(hx, hy)
            z = self.view_z() - 6 + (self.rnd() & 15)
            if s is not None:
                if z < s[0]: z = s[0] + 4
                if z > s[1]: z = s[1] - 4
            self.fx.append([hx, hy, z, b"PUFF", b"ABCD", 0.0, 10])

    def view_z(self):
        s = self.lvl.sectors[self.psec_i]
        return s[0] + EYE

    def spawn_proj(self, owner, base, mult, sx=None, sy=None, sz=None,
                   ang=None):
        if ang is None:
            ang = self.pa
        spd = 700.0 if base == b"PLSS" else 500.0 if base == b"MISL" else 350.0
        if sx is None:
            sx = self.px; sy = self.py; sz = self.view_z() - 8
        self.proj.append([sx, sy, sz, math.cos(ang) * spd, math.sin(ang) * spd,
                          base, 0.0, owner, mult])

    def noise_wake(self):
        for t in self.things_live:
            if t[10] == 0 and MONST.get(t[3]) is not None:
                dx = t[0] - self.px; dy = t[1] - self.py
                if dx * dx + dy * dy < 1000000:        # 1000 units
                    if not self.lvl.rejects(self.psec_i, t[14]):
                        t[10] = 1
                        t[5] = MFRAMES[t[4]][0]
                        t[15] = self.now + 0.9 + (self.rnd() & 127) / 80.0

    def damage_monster(self, t, d):
        ms = MONST.get(t[3])
        if t[3] == 2035:                   # barrel
            t[9] -= d
            if t[9] <= 0 and t[10] < 4:
                self.explode_barrel(t)
            return
        if ms is None:
            return
        t[9] -= d
        if t[10] == 0:
            t[10] = 1                      # wake on pain
        if t[9] <= 0:
            t[10] = 4                      # dying
            t[5] = MFRAMES[t[4]][3]
            t[8] = 0
            t[11] = 0.11
            t[12] = 0
            self.mon_active = True
            self.snd(20 + (self.rnd() & 7), 4)
        elif self.rnd() < ms[3]:
            t[10] = 3
            t[5] = MFRAMES[t[4]][2]
            t[8] = 0
            t[11] = 0.3
            self.snd(44, 2)

    def explode_barrel(self, t):
        t[10] = 4
        t[4] = b"BEXP"
        t[5] = b"ABCDE"
        t[8] = 0
        t[11] = 0.10
        t[12] = 0
        t[9] = 0
        self.snd(20, 6)
        bx, by = t[0], t[1]
        for o in self.things_live:
            if o is t or (o[9] <= 0 and o[3] != 2035) or o[10] >= 4:
                continue
            if MONST.get(o[3]) is None and o[3] != 2035:
                continue
            dx = abs(o[0] - bx); dy = abs(o[1] - by)
            dd = dx if dx > dy else dy
            if dd < 128:
                self.damage_monster(o, 128 - int(dd))
        dx = abs(self.px - bx); dy = abs(self.py - by)
        dd = dx if dx > dy else dy
        if dd < 128:
            self.damage_player(int((128 - dd) * 0.7))

    def damage_player(self, d):
        if self.dead or self.now < self.invuln_until:
            return
        if self.armor > 0:
            ab = d // 3
            if ab > self.armor:
                ab = self.armor
            self.armor -= ab
            d -= ab
        self.health -= d
        self.hud_key = None
        self.snd(33, 3)
        if self.health <= 0:
            self.health = 0
            self.dead = True
            self.msgset("You died.  USE restarts the level.")

    # ---- monsters -----------------------------------------------------------
    def run_monsters(self, dt):
        self.mon_active = False
        px, py = self.px, self.py
        for t in self.things_live:
            ms = MONST.get(t[3])
            if ms is None:
                if t[10] == 4:             # exploding barrel
                    self.mon_active = True
                    t[11] -= dt
                    if t[11] <= 0:
                        t[11] = 0.10
                        t[8] += 1
                        if t[8] >= len(t[5]):
                            self.things_live.remove(t)
                            break
                continue
            st = t[10]
            if st == 5:
                continue
            if st == 4:                    # death animation
                self.mon_active = True
                t[11] -= dt
                if t[11] <= 0:
                    t[11] = 0.11
                    if t[8] < len(t[5]) - 1:
                        t[8] += 1
                    else:
                        t[10] = 5
                continue
            dx = px - t[0]; dy = py - t[1]
            dist2 = dx * dx + dy * dy
            if dist2 > 4000000.0:          # >2000 units: dormant
                continue
            if st == 0:
                t[13] -= dt
                if t[13] <= 0:
                    t[13] = 0.5 + (self.rnd() & 31) / 64.0
                    if not self.lvl.rejects(t[14], self.psec_i):
                        fx_ = math.cos(t[2]); fy = math.sin(t[2])
                        if fx_ * dx + fy * dy > 0 or dist2 < 40000:
                            if self.sight_clear(t[0], t[1], px, py):
                                t[10] = 1
                                t[5] = MFRAMES[t[4]][0]
                                t[15] = self.now + 0.7 + (self.rnd() & 127) / 100.0
                                self.snd(36 + (self.rnd() & 7), 3)
                continue
            self.mon_active = True
            if st == 3:                    # pain
                t[11] -= dt
                if t[11] <= 0:
                    t[10] = 1
                    t[5] = MFRAMES[t[4]][0]
                    t[8] = 0
                continue
            if st == 2:                    # attack wind-up
                t[2] = math.atan2(dy, dx)
                t[11] -= dt
                nfr = len(t[5])
                pr = int((0.4 - t[11]) * nfr * 2.5)
                t[8] = pr if pr < nfr else nfr - 1
                if t[11] <= 0:
                    self.monster_attack(t, ms, dx, dy, dist2)
                    t[10] = 1
                    t[5] = MFRAMES[t[4]][0]
                    t[8] = 0
                    t[15] = self.now + 1.0 + (self.rnd() & 127) / 96.0
                continue
            # chase
            t[13] -= dt
            if t[13] <= 0:
                t[13] = 0.4 + (self.rnd() & 15) / 64.0
                want = math.atan2(dy, dx)
                t[2] = want
                atk = ms[4]
                close = dist2 < 4900       # 70 units: melee range
                ranged = atk != 0 and atk != 6
                if (close or (ranged and self.now >= t[15]
                              and (self.rnd() & 3) == 0)):
                    if close or self.sight_clear(t[0], t[1], px, py):
                        t[10] = 2
                        t[5] = MFRAMES[t[4]][1]
                        t[8] = 0
                        t[11] = 0.4
                        continue
            spd = ms[1] * (2.2 if ms[4] == 6 else 1.0)
            mv = spd * dt
            ca = math.cos(t[2]); sa = math.sin(t[2])
            nx = t[0] + ca * mv; ny = t[1] + sa * mv
            if not self.blocked(t[0], t[1], nx, ny, ms[2], t):
                t[0] = nx; t[1] = ny
                si = self.point_secidx(nx, ny)
                if si != t[14]:
                    t[14] = si
                    t[7] = self.lvl.sectors[si]
                t[11] += dt
                if t[11] > 0.18:           # walk animation
                    t[11] = 0.0
                    t[8] = (t[8] + 1) % len(t[5])
            else:
                t[2] += (0.9 if (self.rnd() & 1) else -0.9)

    def monster_attack(self, t, ms, dx, dy, dist2):
        atk = ms[4]
        dist = math.sqrt(dist2)
        if dist2 < 4900 or atk == 0 or atk == 6:
            if dist2 < 4900:
                self.snd(26, 2)
                self.damage_player((1 + self.rnd() % 10) *
                                   (4 if t[3] == 3002 else 3))
            return
        if atk == 1 or atk == 2:
            if not self.sight_clear(t[0], t[1], self.px, self.py):
                return
            self.snd(52, 2)
            pellets = 3 if atk == 2 else 1
            hitp = 0.45 - dist / 1400.0
            if hitp < 0.08: hitp = 0.08
            for _ in range(pellets):
                if (self.rnd() / 255.0) < hitp:
                    self.damage_player(3 * (1 + self.rnd() % 5))
        else:
            base = b"BAL1" if atk == 3 else b"BAL2" if atk == 4 else b"BAL7"
            mult = 3 if atk == 3 else 5 if atk == 4 else 8
            ang = math.atan2(self.py - t[1], self.px - t[0])
            self.spawn_proj(1, base, mult, t[0], t[1], t[7][0] + 32, ang)
            self.snd(42, 3)

    # ---- projectiles + effects ----------------------------------------------
    def run_proj(self, dt):
        px, py = self.px, self.py
        for p in self.proj[:]:
            p[6] += dt
            if p[6] > 6.0:
                self.proj.remove(p)
                continue
            steps = 2
            sdt = dt / steps
            hit = False
            for _ in range(steps):
                p[0] += p[3] * sdt
                p[1] += p[4] * sdt
                s = self.point_sector(p[0], p[1])
                if s is None or p[2] < s[0] or p[2] > s[1]:
                    hit = True
                    break
                if p[7] == 1:              # monster shot vs player
                    if abs(p[0] - px) < 20 and abs(p[1] - py) < 20:
                        self.damage_player(p[8] * (1 + self.rnd() % 8))
                        hit = True
                        break
                else:                      # player shot vs monsters/barrels
                    for t in self.things_live:
                        if t[9] <= 0 or t[10] >= 4:
                            continue
                        if (abs(t[0] - p[0]) < t[12] + 8 and
                                abs(t[1] - p[1]) < t[12] + 8):
                            self.damage_monster(t, p[8] * (1 + self.rnd() % 8))
                            if p[5] == b"MISL":
                                for o in self.things_live:
                                    if o is t or o[9] <= 0 or o[10] >= 4:
                                        continue
                                    ddx = abs(o[0] - p[0]); ddy = abs(o[1] - p[1])
                                    dd = ddx if ddx > ddy else ddy
                                    if dd < 128:
                                        self.damage_monster(o, (128 - int(dd)) // 2)
                            hit = True
                            break
                    if hit:
                        break
            if hit:
                if p[5] == b"MISL":
                    exp = (b"MISL", b"BCD")
                    self.snd(22, 5)
                elif p[5] == b"PLSS":
                    exp = (b"PLSE", b"ABCDE")
                else:
                    exp = (p[5], b"CDE")
                self.fx.append([p[0], p[1], p[2], exp[0], exp[1], 0.0, 14])
                self.proj.remove(p)

    def run_fx(self, dt):
        for f in self.fx[:]:
            f[5] += dt
            if int(f[5] * f[6]) >= len(f[4]):
                self.fx.remove(f)

    def run_movers(self, dt):
        if not self.movers:
            return
        for mv in self.movers[:]:
            if not mv.tick(dt):
                self.movers.remove(mv)
        self.dirty = True

    def floor_damage(self):
        if self.now < self.dmg_next:
            return
        self.dmg_next = self.now + 1.0
        s = self.lvl.sectors[self.psec_i]
        sp = s[5]
        dmg = 5 if sp == 7 else 10 if sp == 5 else \
            20 if sp in (4, 16) else 0
        if dmg and self.now >= self.suit_until:
            self.damage_player(dmg)
        # E1M8: all barons dead -> lower the tag-666 arena walls
        if self.level == "E1M8" and not self.e1m8_done:
            for t in self.things_live:
                if t[3] == 3003 and t[10] < 4:
                    return
            self.e1m8_done = True
            for si in self.lvl.tagsec.get(666, ()):
                self.start_floor_lower(si)


app = Duum()
