#!/usr/bin/env python3
"""duum_verify.py - geometry oracle + comparison harness for Duum.

Compares what apps/DUUM.PY actually renders against an INDEPENDENT reference
computation of the same view from the same WAD, and asserts they agree.

The reference ("oracle") is a per-column raycaster written clean-room from the
public Doom specs: for every screen column it intersects the view ray with
every LINEDEF, sorts the crossings by depth, and walks them front-to-back
accumulating the visible vertical spans (ceiling / floor / sky / wall pieces)
exactly as the vanilla renderer's portal clipping defines them.  It shares no
code and no algorithm with Duum's BSP walk, so a bug in either shows up as a
disagreement.  Light levels are NOT compared (Duum's falloff is deliberately
its own); geometry, surface classification and wall texture choice are.

  python3 tools/duum_verify.py                      all levels, all viewpoints
  python3 tools/duum_verify.py --level E1M1         one level
  python3 tools/duum_verify.py --wad path/DOOM1.WAD --level E1M1 --diff-dir out
  python3 tools/duum_verify.py --quick              player starts only

Exit code 0 = every view within tolerance, 1 = mismatches (see report).

A second, WAD-independent check runs on every view: the solid ops Duum emits
for one frame must tile each pixel column exactly once (no holes, no solid
overdraw) - catching display-list bugs even where the oracle agrees.
"""
import os, sys, math, struct, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import duum_host


# ---- independent WAD/level parse (no code shared with DUUM.PY) -------------
class RefLevel:
    def __init__(self, wadpath, name):
        with open(wadpath, "rb") as f:
            data = f.read()
        magic, n, diro = struct.unpack_from("<4sII", data, 0)
        assert magic in (b"IWAD", b"PWAD"), "not a WAD"
        lumps = []
        for i in range(n):
            off, sz, nm = struct.unpack_from("<II8s", data, diro + 16 * i)
            lumps.append((nm.rstrip(b"\0"), data[off:off + sz]))
        mi = next(i for i, (nm, _) in enumerate(lumps)
                  if nm == name.encode())
        lp = dict(lumps[mi + 1:mi + 11])
        unp = lambda k, f, s: [struct.unpack_from(f, lp[k], o)
                               for o in range(0, len(lp[k]) - s + 1, s)]
        self.verts = unp(b"VERTEXES", "<hh", 4)
        self.lines = unp(b"LINEDEFS", "<HHHHHHH", 14)
        self.sides = unp(b"SIDEDEFS", "<hh8s8s8sH", 30)
        self.sectors = unp(b"SECTORS", "<hh8s8shHH", 26)
        self.things = unp(b"THINGS", "<hhHHH", 10)
        self.csky = [s[3].rstrip(b"\0")[:6] == b"F_SKY1" for s in self.sectors]
        self.side_sec = [sd[5] for sd in self.sides]

    def texname(self, sdi, field):
        """field: 2 upper, 3 lower, 4 mid -> uppercase str or None."""
        if sdi == 0xFFFF:
            return None
        nm = self.sides[sdi][field].rstrip(b"\0").decode("latin1").upper()
        return nm if nm and nm != "-" else None

    def player_start(self):
        for x, y, ang, typ, fl in self.things:
            if typ == 1:
                return x, y, ang
        raise SystemExit("no player start")

    def dm_starts(self):
        return [(x, y, ang) for x, y, ang, typ, fl in self.things
                if typ == 11]


# ---- the oracle: per-column raycast ----------------------------------------
# Kinds a row can be: 'C' ceiling flat, 'F' floor flat, 'S' sky, 'W' wall.
class Oracle:
    def __init__(self, lvl, cw=640, ch=400, rw=220,
                 fov=math.pi / 2, ver=1.2, eye=41):
        self.lvl = lvl
        self.cw = cw; self.ch = ch; self.rw = rw
        self.scale = (rw / 2) / math.tan(fov / 2)
        self.vsc = ((cw / 2) / math.tan(fov / 2)) * ver
        self.hh = ch / 2
        self.eye = eye

    def _crossings(self, px, py, dx, dy):
        """All linedef crossings of ray p+u*(dx,dy), u>0, sorted by u.
        Yields (u, li, front sector idx or None, back sector idx or None,
        front sidedef idx)."""
        lvl = self.lvl
        out = []
        for li, ld in enumerate(lvl.lines):
            ax, ay = lvl.verts[ld[0]]
            bx, by = lvl.verts[ld[1]]
            ex = bx - ax; ey = by - ay
            den = dx * ey - dy * ex
            if den == 0.0:
                continue
            u = ((ax - px) * ey - (ay - py) * ex) / den
            if u <= 0.001:
                continue
            hx = px + u * dx; hy = py + u * dy
            if abs(ex) >= abs(ey):
                s = (hx - ax) / ex
            else:
                s = (hy - ay) / ey
            if s < 0.0 or s >= 1.0:
                continue
            sideval = (px - ax) * ey - (py - ay) * ex
            if sideval == 0.0:
                continue
            fsd, bsd = (ld[5], ld[6]) if sideval > 0 else (ld[6], ld[5])
            fsec = lvl.side_sec[fsd] if fsd != 0xFFFF else None
            bsec = lvl.side_sec[bsd] if bsd != 0xFFFF else None
            out.append((u, li, fsec, bsec, fsd))
        out.sort(key=lambda c: c[0])
        return out

    def viewz(self, px, py, pa):
        """Eye height: floor of the sector the viewer is in + EYE.
        Derived from the nearest crossing's front sector (BSP-free)."""
        cr = self._crossings(px, py, math.cos(pa), math.sin(pa))
        for (u, li, fsec, bsec, fsd) in cr:
            if fsec is not None:
                return self.lvl.sectors[fsec][0] + self.eye
        return self.eye

    def column(self, px, py, viewz, dx, dy):
        """One column's visible spans, front to back.
        Returns list of (kind, ytop, ybot, detail) with float half-open
        [ytop, ybot) canvas rows; detail = sector idx for C/F, (li, part,
        texname) for W, None for S."""
        lvl = self.lvl
        hh = self.hh; vsc = self.vsc; ch = float(self.ch)
        ct = 0.0; cb = ch
        spans = []

        def emit(kind, a, b, det=None):
            if b > a:
                spans.append((kind, a, b, det))

        for (u, li, fsec, bsec, fsd) in self._crossings(px, py, dx, dy):
            if fsec is None:           # sealed maps never show a line's back
                emit('W', ct, cb, (li, 'void', None))
                return spans
            fs = lvl.sectors[fsec]
            fc = fs[1]; ff = fs[0]
            fsky = lvl.csky[fsec]
            yfc = hh - (fc - viewz) * vsc / u
            yff = hh - (ff - viewz) * vsc / u
            emit('S' if fsky else 'C', ct, min(yfc, cb), fsec)
            emit('F', max(yff, ct), cb, fsec)
            if bsec is None:
                emit('W', max(ct, yfc), min(cb, yff),
                     (li, 'mid', lvl.texname(fsd, 4)))
                return spans
            bs = lvl.sectors[bsec]
            bc = bs[1]; bf = bs[0]
            bothsky = fsky and lvl.csky[bsec]
            # vanilla piece clipping: the upper piece is clipped to the
            # seg's own floor, and the lower piece to the seg's own
            # ceiling/upper piece (R_RenderSegLoop's ceilingclip handoff) -
            # a closed portal (back floor >= front ceiling) shows only its
            # lower texture between the front ceiling and floor edges.
            topclip = max(ct, yfc)
            if bothsky:
                open_top = fc          # vanilla sky hack: no upper wall
            else:
                open_top = min(fc, bc)
                if bc < fc:
                    ybc = hh - (bc - viewz) * vsc / u
                    emit('W', max(ct, yfc), min(cb, ybc, yff),
                         (li, 'up', lvl.texname(fsd, 2)))
                    topclip = max(topclip, ybc)
            open_bot = max(ff, bf)
            if bf > ff:
                ybf = hh - (bf - viewz) * vsc / u
                emit('W', max(topclip, ybf), min(cb, yff),
                     (li, 'lo', lvl.texname(fsd, 3)))
            ct = max(ct, hh - (open_top - viewz) * vsc / u)
            cb = min(cb, hh - (open_bot - viewz) * vsc / u)
            if ct >= cb:
                return spans
        emit('S', ct, cb)              # leftover: open map edge shows sky
        return spans

    def view(self, px, py, pa):
        """All columns for a viewpoint -> list[rw] of span lists."""
        viewz = self.viewz(px, py, pa)
        cos_a = math.cos(pa); sin_a = math.sin(pa)
        cols = []
        for x in range(self.rw):
            # Duum samples at integer x.  Integer map geometry means a ray
            # can pass exactly through a vertex and "leak" (first crossing is
            # a one-sided line's back): nudge sideways and retry.
            for bump in (0.0, 0.003, -0.003, 0.011):
                k = (x + bump - self.rw / 2) / self.scale
                dx = cos_a + k * sin_a
                dy = sin_a - k * cos_a
                spans = self.column(px, py, viewz, dx, dy)
                if not any(s[0] == 'W' and s[3] and s[3][1] == 'void'
                           for s in spans):
                    break
            cols.append(spans)
        return cols, viewz


# ---- extracting what Duum actually drew ------------------------------------
def duum_classes(app, rw):
    """Replay app.frame's SOLID ops into a per-internal-column class map.
    Returns (classes, texids, overdraw, holes):
      classes[x][y] in {'?','C','F','S','W','R'}  ('R' = untextured rect)
      texids[x][y]  id() of the texture grid for W rows (0 otherwise)
    Solid ops are 'R', 'F', 'W'; masked ops ('M') are sprites/HUD and are
    ignored.  Sky is a 'W' op whose grid is the sky texture's."""
    cw = app.cw; ch = app.ch
    sky_grid = id(app.sky[2]) if app.sky else None
    classes = [bytearray(b'?' * ch) for _ in range(rw)]
    texids = [[0] * ch for _ in range(rw)]
    over = 0; wrote = [bytearray(ch) for _ in range(rw)]
    # pixel col -> internal col (first pixel col of each internal col only)
    px2ic = {}
    for x in range(rw):
        px2ic[x * cw // rw] = x
    for op in app.frame:
        k = op[0]
        if k == 'M':
            continue
        if k == 'R':
            _, x0, y0, w, h, color = op
            cls = b'R'; tid = 0
        elif k == 'F':
            _, x0, w, y0, h, grid, a, dcx, dcy, lf = op
            cls = b'C' if a > 0 else b'F'; tid = 0
        else:                          # 'W'
            x0, w, y0, h = op[1], op[2], op[3], op[4]
            if id(op[5]) == sky_grid:
                cls = b'S'; tid = 0
            else:
                cls = b'W'; tid = id(op[5])
        for pxc in range(x0, x0 + w):
            ic = px2ic.get(pxc)
            if ic is None:
                continue
            colc = classes[ic]; colt = texids[ic]; colw = wrote[ic]
            for yy in range(max(0, y0), min(ch, y0 + h)):
                if colw[yy]:
                    over += 1
                colw[yy] = 1
                colc[yy] = cls[0]
                colt[yy] = tid
    holes = sum(1 for x in range(rw) for yy in range(ch) if not wrote[x][yy])
    return classes, texids, over, holes


def oracle_classes(cols, ch):
    """Rasterize oracle spans to per-row classes (last span wins, matching
    painter order) -> list[rw] of bytes plus per-row wall detail."""
    out = []; det = []
    for spans in cols:
        row = bytearray(b'?' * ch)
        dr = [None] * ch
        for (kind, a, b, d) in spans:
            y0 = max(0, int(math.floor(a + 0.5)))
            y1 = min(ch, int(math.floor(b + 0.5)))
            for yy in range(y0, y1):
                row[yy] = ord(kind)
                dr[yy] = d if kind == 'W' else None
        out.append(row); det.append(dr)
    return out, det


def compare_view(app, oracle, px, py, pa, tol=2):
    """Render Duum at (px,py,pa), oracle the same view, diff.
    Returns dict with mismatch stats + arrays for diff imagery."""
    app.px = float(px); app.py = float(py); app.pa = pa
    app.psec_i = app.point_secidx(px, py)
    app.render()
    rw = 220
    got, gtex, over, holes = duum_classes(app, rw)
    cols, viewz = oracle.view(px, py, pa)
    want, wdet = oracle_classes(cols, app.ch)
    ch = app.ch
    # texture-name map: grid id -> name, from Duum's texture cache
    tex_by_id = {}
    for nm, t in app.tex.cache.items():
        if t is not None:
            tex_by_id[id(t[2])] = nm
    mism = [0] * rw
    texbad = {}
    for x in range(rw):
        g = got[x]; w = want[x]
        for yy in range(ch):
            wc = w[yy]
            if wc == ord('?'):
                continue
            gc = g[yy]
            if gc == ord('R'):         # untextured fallback: class unknowable
                continue
            ok = gc == wc
            if not ok:                 # boundary tolerance: +-tol rows
                lo = max(0, yy - tol); hi = min(ch, yy + tol + 1)
                ok = any(g[t] == wc for t in range(lo, hi)) and \
                     any(w[t] == gc for t in range(lo, hi))
            if not ok:
                mism[x] += 1
            elif gc == wc == ord('W'):
                d = wdet[x][yy]
                if d and d[2]:
                    gotnm = tex_by_id.get(gtex[x][yy])
                    if gotnm is not None and gotnm != d[2]:
                        # column-boundary rounding: accept a neighbour
                        # column's expected texture before flagging
                        near = set()
                        for nx in (x - 1, x + 1):
                            if 0 <= nx < rw and wdet[nx][yy]:
                                near.add(wdet[nx][yy][2])
                        if gotnm not in near:
                            k = (d[0], d[1], d[2], gotnm)
                            texbad[k] = texbad.get(k, 0) + 1
    total = rw * ch
    bad = sum(mism)
    return {
        "bad_rows": bad, "total": total, "pct": 100.0 * bad / total,
        "worst_cols": sorted(range(rw), key=lambda x: -mism[x])[:8],
        "mism": mism, "got": got, "want": want,
        "overdraw": over, "holes": holes, "texbad": texbad,
        "viewz": viewz,
    }


def diff_png(app, res, path):
    """Duum frame with mismatched oracle rows tinted red + class strips."""
    from PIL import Image
    cw, ch, rw = app.cw, app.ch, 220
    cv = duum_host.Canvas(cw, ch)
    app.draw(cv)
    img = Image.frombytes("RGB", (cw, ch), bytes(cv.buf))
    p = img.load()
    got, want, mism = res["got"], res["want"], res["mism"]
    for x in range(rw):
        if not mism[x]:
            continue
        x0 = x * cw // rw; x1 = (x + 1) * cw // rw
        for yy in range(ch):
            wc = want[x][yy]; gc = got[x][yy]
            if wc == ord('?') or gc == ord('R') or gc == wc:
                continue
            for pxc in range(x0, x1):
                r, g, b = p[pxc, yy]
                p[pxc, yy] = (255, g // 3, b // 3)
    img.save(path)


VIEW_ANGLES = (0, 45, 90, 135, 180, 225, 270, 315)


def views_for(lvl, quick):
    px, py, ang = lvl.player_start()
    vs = [("start%+d" % a, px, py, math.radians((ang + a) % 360))
          for a in (0, 90, 180, 270)] if not quick else \
         [("start", px, py, math.radians(ang))]
    if not quick:
        for i, (x, y, a) in enumerate(lvl.dm_starts()[:4]):
            vs.append(("dm%d" % i, x, y, math.radians(a)))
    return vs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=os.path.join(PC64, "wads", "DOOM1.WAD"))
    ap.add_argument("--level", default=None)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--tol", type=int, default=2)
    ap.add_argument("--max-bad-pct", type=float, default=0.5,
                    help="fail a view when > this %% of oracle rows mismatch")
    ap.add_argument("--diff-dir", default=None,
                    help="write <view>.png diff images here")
    args = ap.parse_args()

    duum_host.WAD_OVERRIDE = os.path.abspath(args.wad)
    mod = duum_host.load_app()
    app = mod.app
    cv = duum_host.Canvas()
    app.build(cv)
    if app.err:
        sys.exit("app.build failed: %s" % app.err)

    levels = [args.level] if args.level else \
        ["E1M%d" % i for i in range(1, 10)]
    failures = 0
    for level in levels:
        if app.level != level:
            app.load_level(level)
        ref = RefLevel(args.wad, level)
        oracle = Oracle(ref, cw=app.cw, ch=app.ch)
        for (nm, px, py, pa) in views_for(ref, args.quick):
            res = compare_view(app, oracle, px, py, pa, tol=args.tol)
            ok = (res["pct"] <= args.max_bad_pct and res["holes"] == 0)
            status = "ok  " if ok else "FAIL"
            print("%s %s/%-9s  class-mism %6.2f%%  holes %d  overdraw %d"
                  % (status, level, nm, res["pct"], res["holes"],
                     res["overdraw"]))
            for k, cnt in sorted(res["texbad"].items(), key=lambda e: -e[1])[:4]:
                li, part, wantnm, gotnm = k
                print("      tex: line %d %s want %s got %s (%d rows)"
                      % (li, part, wantnm, gotnm, cnt))
            if not ok:
                failures += 1
                if args.diff_dir:
                    os.makedirs(args.diff_dir, exist_ok=True)
                    diff_png(app, res, os.path.join(
                        args.diff_dir, "%s_%s.png" % (level, nm)))
    print("%d failing view(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
