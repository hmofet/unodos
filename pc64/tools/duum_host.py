#!/usr/bin/env python3
"""Duum host harness - runs the REAL app (apps/DUUM.PY) on desktop Python.

Earlier this file was a parallel reimplementation of the renderer; it drifted
from the app (and shared its bugs).  Now it is a shim: it fabricates the `uno`
module (WAD-backed file I/O + a PIL-backed Canvas with the same fast-path
methods the C side exposes) and imports apps/DUUM.PY unmodified, so what you
see on the host is what the device runs, minus MicroPython speed.

  python3 tools/duum_host.py shot out.png            one frame from the player start
  python3 tools/duum_host.py shot out.png --keys UULLUU   after a key script
  python3 tools/duum_host.py walk out_dir N          N frames walking forward
  python3 tools/duum_host.py map out.png             top-down linedef map
  python3 tools/duum_host.py bench                   time a render() on the host
  python3 tools/duum_host.py play [--scale N]        PLAY it in a window (tkinter)

Global flags: --wad PATH serves that file for every WAD name (test any IWAD
without touching wads/), --level E1M3 starts on another map.  The geometry
test suite is tools/duum_verify.py; tools/duum_vs_doom.py makes side-by-side
sheets against Chocolate Doom.

Key script letters: U/D = forward/back, L/R = turn, ,/. = strafe, SPACE = use,
F = fire, 1..9 = weapon select.
"""
import os, sys, math, struct, time, types, importlib.util, importlib.machinery

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
WADDIR = os.path.join(PC64, "wads")
CW, CH = 640, 400        # host canvas: same aspect ballpark as the device window


# ---- the fake `uno` module -------------------------------------------------
WAD_OVERRIDE = None      # --wad PATH: serve this file for every WAD name


def _wad_path(name):
    if WAD_OVERRIDE:
        return WAD_OVERRIDE
    p = os.path.join(WADDIR, name)
    if os.path.exists(p):
        return p
    # the device stages freedoom1.wad as DOOM1.WAD; accept either on the host
    for alt in os.listdir(WADDIR):
        if alt.upper() == name.upper() or alt.lower() == "freedoom1.wad":
            return os.path.join(WADDIR, alt)
    return p


class _Files:
    _h = {}
    @classmethod
    def handle(cls, name):
        f = cls._h.get(name)
        if f is None:
            f = open(_wad_path(name), "rb")
            cls._h[name] = f
        return f


def uno_size(vol, name=None):
    if name is None:
        name = vol
    try:
        return os.path.getsize(_wad_path(name))
    except OSError:
        return -1


def uno_read_at(vol, name, off, n):
    f = _Files.handle(name)
    f.seek(off)
    return f.read(n)


class Canvas:
    """Mirrors the device canvas contract in mod_uno.c (incl. wall_col and the
    helpers Duum grows; keep the two in sync)."""
    def __init__(self, w=CW, h=CH):
        self.w = w; self.h = h
        self.buf = bytearray(w * h * 3)      # RGB
        self.texts = []                       # (x, y, s, color) - play mode

    def width(self):  return self.w
    def height(self): return self.h

    @staticmethod
    def _rgb(color):
        return (color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF)

    def clear(self, color):
        r, g, b = self._rgb(color)
        self.buf[:] = bytes((r, g, b)) * (self.w * self.h)
        self.texts = []

    def fill_rect(self, x, y, w, h, color):
        r, g, b = self._rgb(color)
        x0 = max(0, x); y0 = max(0, y)
        x1 = min(self.w, x + w); y1 = min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes((r, g, b)) * (x1 - x0)
        for yy in range(y0, y1):
            base = (yy * self.w + x0) * 3
            self.buf[base:base + len(row)] = row

    def rect(self, x, y, w, h, color):
        self.fill_rect(x, y, w, 1, color); self.fill_rect(x, y + h - 1, w, 1, color)
        self.fill_rect(x, y, 1, h, color); self.fill_rect(x + w - 1, y, 1, h, color)

    def pixel(self, x, y, color):
        if 0 <= x < self.w and 0 <= y < self.h:
            base = (y * self.w + x) * 3
            self.buf[base:base + 3] = bytes(self._rgb(color))

    def hline(self, x, y, w, color):
        self.fill_rect(x, y, w, 1, color)

    def vline(self, x, y, h, color):
        self.fill_rect(x, y, 1, h, color)

    def text(self, x, y, s, color):
        self.texts.append((x, y, s, color))   # play mode overlays these

    def wall_span(self, x, w, y0, count, grid, tw, th, tc, v0, dv, pal, sh):
        for k in range(w):
            self.wall_col(x + k, y0, count, grid, tw, th, tc, v0, dv, pal, sh)

    def mask_span(self, x, w, y0, count, grid, tw, th, tc, v0, dv, pal, sh):
        for k in range(w):
            self.mask_col(x + k, y0, count, grid, tw, th, tc, v0, dv, pal, sh)

    def flat_span(self, x, w, y0, count, grid, pal, a, ycen, dx, dy, wx, wy, lf):
        for k in range(w):
            self.flat_col(x + k, y0, count, grid, pal, a, ycen, dx, dy, wx, wy, lf)

    def wall_col(self, x, y0, count, grid, tw, th, texcol, v0, dv, pal, sh):
        """Byte-faithful mirror of cv_wall_col in mod_uno.c."""
        if sh > 256:
            sh = 256
        if tw <= 0 or th <= 0 or count <= 0:
            return
        texcol %= tw
        base_t = texcol * th
        v = v0
        w = self.w
        y1 = min(y0 + count, self.h)
        yy = y0
        buf = self.buf
        while yy < y1:
            if yy >= 0:
                vv = (v >> 8) % th
                pi = grid[base_t + vv] * 3
                base = (yy * w + x) * 3
                buf[base]     = (pal[pi] * sh) >> 8
                buf[base + 1] = (pal[pi + 1] * sh) >> 8
                buf[base + 2] = (pal[pi + 2] * sh) >> 8
            v += dv
            yy += 1

    def mask_col(self, x, y0, count, grid, tw, th, texcol, v0, dv, pal, sh):
        """wall_col with a transparent sentinel (0xFF) and NO vertical wrap:
        mirror of the planned cv_mask_col in mod_uno.c."""
        if tw <= 0 or th <= 0 or count <= 0:
            return
        texcol %= tw
        base_t = texcol * th
        v = v0
        w = self.w
        thfp = th << 8
        y1 = min(y0 + count, self.h)
        yy = y0
        buf = self.buf
        while yy < y1:
            if yy >= 0 and 0 <= v < thfp:
                pi = grid[base_t + (v >> 8)]
                if pi != 0xFF:
                    pi *= 3
                    base = (yy * w + x) * 3
                    buf[base]     = (pal[pi] * sh) >> 8
                    buf[base + 1] = (pal[pi + 1] * sh) >> 8
                    buf[base + 2] = (pal[pi + 2] * sh) >> 8
            v += dv
            yy += 1

    def flat_col(self, x, y0, count, grid, pal, a, ycen, dirx, diry, wx0, wy0, lf):
        """Perspective flat mapper: mirror of the planned cv_flat_col in
        mod_uno.c.  a = (plane_height - viewz) * vscale; per pixel
        dist = a / (ycen - y - 0.5), world = view + dir * dist, texel 64x64."""
        buf = self.buf
        w = self.w
        y1 = min(y0 + count, self.h)
        yy = max(y0, 0)
        while yy < y1:
            yd = ycen - (yy + 0.5)
            if yd != 0.0:
                dist = a / yd
                wx = wx0 + dirx * dist
                wy = wy0 + diry * dist
                ix = int(wx); ix = ix - 1 if wx < ix else ix     # floor()
                iy = int(wy); iy = iy - 1 if wy < iy else iy
                ti = (((-iy) & 63) << 6) | (ix & 63)
                df = 1200.0 / (dist + 650.0)
                if df > 1.0: df = 1.0
                elif df < 0.68: df = 0.68
                sh = int(lf * df * 256.0)
                pi = grid[ti] * 3
                base = (yy * w + x) * 3
                buf[base]     = (pal[pi] * sh) >> 8
                buf[base + 1] = (pal[pi + 1] * sh) >> 8
                buf[base + 2] = (pal[pi + 2] * sh) >> 8
            yy += 1

    def save(self, path):
        from PIL import Image
        img = Image.frombytes("RGB", (self.w, self.h), bytes(self.buf))
        img.save(path)
        return path


class App:
    def build(self, cv): pass
    def draw(self, cv): pass
    def tick(self): pass
    def key(self, uni, scan, ctrl): return False
    def opened(self): pass
    def closed(self): pass


def make_uno():
    m = types.ModuleType("uno")
    m.App = App
    m.size = uno_size
    m.read_at = uno_read_at
    m.read = lambda *a: open(_wad_path(a[-1]), "rb").read()
    m.rgb = lambda r, g, b: 0xFF000000 | (min(b,255) << 16) | (min(g,255) << 8) | min(r,255)
    m.beep = lambda midi, ticks: None
    m.quiet = lambda: None
    # scripted clock: the harness advances it explicitly so tests are
    # deterministic; ticks() is the same 60Hz counter the device exposes
    m._t = [0.0]
    m.ticks = lambda: int(m._t[0] * 60)
    m.advance = lambda dt: m._t.__setitem__(0, m._t[0] + dt)
    m.keys_down = lambda: 0
    return m


def load_app():
    sys.modules["uno"] = make_uno()
    spec = importlib.util.spec_from_loader(
        "duum", importlib.machinery.SourceFileLoader(
            "duum", os.path.join(PC64, "apps", "DUUM.PY")))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# UnoDOS scancodes: Up=1 Down=2 Right=3 Left=4 (hid_kbd.h)
KEYS = {"U": (0, 1), "D": (0, 2), "R": (0, 3), "L": (0, 4),
        ",": (ord(","), 0), ".": (ord("."), 0), " ": (ord(" "), 0),
        "F": (ord("f"), 0)}


def run_keys(app, script):
    """Key letters press keys; each step then simulates 3 game ticks at 30Hz.
    'W' waits half a second without input."""
    uno = sys.modules["uno"]
    for k in script:
        steps = 3
        if k == "W":
            steps = 15
        elif k in KEYS:
            uni, scan = KEYS[k]
            app.key(uni, scan, 0)
        elif k.isdigit():
            app.key(ord(k), 0, 0)
        else:
            continue
        for _ in range(steps):
            uno.advance(1.0 / 30.0)
            app.tick()


def run_play(mod, scale=2):
    """Interactive window: the real DUUM.PY, desktop Python, live keys.
    320x200 canvas (pure-Python pixel replay is the frame budget) scaled up
    with nearest-neighbour.  Arrows move/turn, comma/period strafe, F or
    Ctrl fire, Space/E use, 1-6 weapons, Esc quits."""
    import tkinter as tk
    from PIL import Image, ImageTk, ImageDraw

    app = mod.app
    cv = Canvas(320, 200)
    app.build(cv)
    if app.err:
        sys.exit("app.build failed: %s" % app.err)
    uno = sys.modules["uno"]
    t0 = time.monotonic()
    uno.ticks = lambda: int((time.monotonic() - t0) * 60)   # real 60Hz clock

    # live held-key state -> uno.keys_down bitmask (UNO_KH_* bits)
    BITS = {"Up": 1, "Down": 2, "Right": 4, "Left": 8,
            "f": 16, "F": 16, "space": 32, "e": 32, "E": 32,
            "comma": 64, "period": 128}
    kd = [0]
    uno.keys_down = lambda: kd[0]
    app.have_keys = True

    root = tk.Tk()
    root.title("Duum - %s (host)" % mod.LEVEL)
    W, H = 320 * scale, 200 * scale
    label = tk.Label(root, width=W, height=H, bd=0)
    label.pack()

    def press(ev):
        ks = ev.keysym
        if ks == "Escape":
            root.destroy()
            return
        b = BITS.get(ks)
        if b:
            kd[0] |= b
        if ks in ("space", "Return"):
            app.key(32, 0, 0)          # restart hooks when dead/finished
        elif len(ks) == 1 and ks.isdigit():
            app.key(ord(ks), 0, 0)

    def release(ev):
        b = BITS.get(ev.keysym)
        if b:
            kd[0] &= ~b

    root.bind("<KeyPress>", press)
    root.bind("<KeyRelease>", release)
    holder = {}

    def frame():
        drew = app.tick()
        if drew or "ph" not in holder:
            app.draw(cv)
            img = Image.frombytes("RGB", (320, 200), bytes(cv.buf))
            img = img.resize((W, H), Image.NEAREST)
            if cv.texts:
                d = ImageDraw.Draw(img)
                for (x, y, s, color) in cv.texts:
                    d.text((x * scale, y * scale), s,
                           fill=Canvas._rgb(color))
            ph = ImageTk.PhotoImage(img)
            label.configure(image=ph)
            holder["ph"] = ph
        root.after(15, frame)

    frame()
    root.mainloop()


def main():
    global WAD_OVERRIDE
    if "--wad" in sys.argv:
        i = sys.argv.index("--wad")
        WAD_OVERRIDE = os.path.abspath(sys.argv[i + 1])
        del sys.argv[i:i + 2]
    if "--level" in sys.argv:
        i = sys.argv.index("--level")
        lvl = sys.argv[i + 1]
        del sys.argv[i:i + 2]
    else:
        lvl = None
    cmd = sys.argv[1] if len(sys.argv) > 1 else "shot"
    mod = load_app()
    if lvl:
        mod.LEVEL = lvl

    if cmd == "play":
        sc = 2
        if "--scale" in sys.argv:
            sc = int(sys.argv[sys.argv.index("--scale") + 1])
        run_play(mod, sc)
        return
    app = mod.app
    cv = Canvas()

    if cmd == "map":
        out = sys.argv[2] if len(sys.argv) > 2 else "duum_map.png"
        wad = mod.Wad(mod.WADNAME)
        lvl = mod.Level(wad, mod.LEVEL)
        render_map(lvl, out)
        return

    app.build(cv)
    if app.err:
        sys.exit("app.build failed: %s" % app.err)

    if cmd == "bench":
        app.render()
        t0 = time.perf_counter(); n = 5
        for _ in range(n):
            app.render()
        dt = (time.perf_counter() - t0) / n
        print("render(): %.1f ms/frame on host CPython" % (dt * 1e3))
        return

    if cmd == "shot":
        out = sys.argv[2] if len(sys.argv) > 2 else "duum_shot.png"
        if "--pos" in sys.argv:
            i = sys.argv.index("--pos")
            app.px = float(sys.argv[i + 1]); app.py = float(sys.argv[i + 2])
            app.pa = math.radians(float(sys.argv[i + 3]))
            app.dirty = True
        if "--keys" in sys.argv:
            run_keys(app, sys.argv[sys.argv.index("--keys") + 1])
        app.tick()
        app.draw(cv)
        print("wrote", cv.save(out))
        return

    if cmd == "walk":
        outdir = sys.argv[2] if len(sys.argv) > 2 else "duum_walk"
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
        os.makedirs(outdir, exist_ok=True)
        app.tick(); app.draw(cv)
        cv.save(os.path.join(outdir, "f000.png"))
        for i in range(1, n):
            app.key(0, 1, 0)          # forward
            app.tick(); app.draw(cv)
            cv.save(os.path.join(outdir, "f%03d.png" % i))
        print("wrote %d frames to %s" % (n, outdir))
        return

    sys.exit("unknown command %r" % cmd)


def render_map(lvl, out):
    from PIL import Image, ImageDraw
    xs = [v[0] for v in lvl.verts]; ys = [v[1] for v in lvl.verts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    W, H = 1000, 1000
    s = min((W - 20) / (maxx - minx), (H - 20) / (maxy - miny))
    img = Image.new("RGB", (W, H), (12, 12, 16))
    d = ImageDraw.Draw(img)
    def tx(x): return 10 + (x - minx) * s
    def ty(y): return H - 10 - (y - miny) * s
    for ln in lvl.lines:
        v1, v2 = ln[0], ln[1]
        col = (255, 255, 255) if ln[6] == 0xFFFF else (100, 100, 130)
        d.line([tx(lvl.verts[v1][0]), ty(lvl.verts[v1][1]),
                tx(lvl.verts[v2][0]), ty(lvl.verts[v2][1])], fill=col)
    px, py, pa = lvl.player_start()
    d.ellipse([tx(px) - 4, ty(py) - 4, tx(px) + 4, ty(py) + 4], fill=(255, 80, 80))
    d.line([tx(px), ty(py), tx(px + 40 * math.cos(math.radians(pa))),
            ty(py + 40 * math.sin(math.radians(pa)))], fill=(255, 80, 80))
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
