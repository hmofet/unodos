#!/usr/bin/env python3
"""Generate the committed demo pictures in pc64/pictures/ (the Photos app's
out-of-box content, staged to build/esp/PICTURES/ by build.sh - the same
arrangement as media/ for the Music app).

Everything is drawn procedurally right here, so the files carry no third-party
rights at all: they are this repo's own work, dedicated CC0 (see
pictures/README.TXT). PNG/BMP/TGA/PGM/QOI are written by hand from the specs
using only the Python stdlib; JPG and GIF are converted from those frames with
ImageMagick `convert` (a dev-machine tool; the OUTPUT is what ships). One
format per file on purpose - together they exercise every unomedia decoder.

Run once (WSL: python3 tools/mkdemo_pics.py) and commit the results.
"""
import math, os, struct, subprocess, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(os.path.dirname(HERE), "pictures")
os.makedirs(OUT, exist_ok=True)

# ---- tiny canvas ----------------------------------------------------------
class Img:
    def __init__(self, w, h, bg=(0, 0, 0, 255)):
        self.w, self.h = w, h
        self.p = [list(bg) for _ in range(w * h)]
    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            a = c[3] / 255.0 if len(c) > 3 else 1.0
            d = self.p[y * self.w + x]
            for i in range(3):
                d[i] = int(c[i] * a + d[i] * (1 - a) + 0.5)
            d[3] = max(d[3], c[3] if len(c) > 3 else 255)
    def disc(self, cx, cy, r, c):
        for y in range(int(cy - r), int(cy + r) + 1):
            for x in range(int(cx - r), int(cx + r) + 1):
                d = math.hypot(x - cx, y - cy)
                if d <= r:
                    aa = min(1.0, r - d) if r - d < 1 else 1.0
                    cc = list(c[:3]) + [int((c[3] if len(c) > 3 else 255) * aa)]
                    self.set(x, y, cc)
    def rgba(self):
        return b"".join(bytes(px) for px in self.p)
    def rgb(self):
        return b"".join(bytes(px[:3]) for px in self.p)

# ---- format writers (spec-straight, stdlib only) --------------------------
def write_png(path, im, alpha=True):
    raw = b""
    for y in range(im.h):
        raw += b"\0" + b"".join(
            bytes(im.p[y * im.w + x][:4 if alpha else 3]) for x in range(im.w))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    hdr = struct.pack(">IIBBBBB", im.w, im.h, 8, 6 if alpha else 2, 0, 0, 0)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr)
                           + chunk(b"IDAT", zlib.compress(raw, 9))
                           + chunk(b"IEND", b""))

def write_bmp(path, im):
    stride = (im.w * 3 + 3) & ~3
    px = b""
    for y in range(im.h - 1, -1, -1):
        row = b"".join(bytes((im.p[y*im.w+x][2], im.p[y*im.w+x][1],
                              im.p[y*im.w+x][0])) for x in range(im.w))
        px += row + b"\0" * (stride - im.w * 3)
    open(path, "wb").write(
        b"BM" + struct.pack("<IHHI", 54 + len(px), 0, 0, 54)
        + struct.pack("<IiiHHIIiiII", 40, im.w, im.h, 1, 24, 0,
                      len(px), 2835, 2835, 0, 0) + px)

def write_tga(path, im):
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                      im.w, im.h, 24, 0x20)   # top-left origin
    px = b"".join(bytes((p[2], p[1], p[0])) for p in im.p)
    open(path, "wb").write(hdr + px)

def write_pgm(path, im):
    data = bytes((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8 for p in im.p)
    open(path, "wb").write(b"P5\n%d %d\n255\n" % (im.w, im.h) + data)

def write_qoi(path, im):
    out = bytearray(b"qoif" + struct.pack(">IIBB", im.w, im.h, 4, 0))
    idx = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255); run = 0
    def h(p): return (p[0]*3 + p[1]*5 + p[2]*7 + p[3]*11) % 64
    for q in im.p:
        p = tuple(q)
        if p == prev:
            run += 1
            if run == 62: out.append(0xC0 | 61); run = 0
            continue
        if run: out.append(0xC0 | (run - 1)); run = 0
        if idx[h(p)] == p: out.append(h(p))
        elif p[3] == prev[3]:
            dr = (p[0]-prev[0]) & 0xFF; dg = (p[1]-prev[1]) & 0xFF
            db = (p[2]-prev[2]) & 0xFF
            if (dr+2) & 0xFF < 4 and (dg+2) & 0xFF < 4 and (db+2) & 0xFF < 4:
                out.append(0x40 | (((dr+2) & 3) << 4) | (((dg+2) & 3) << 2)
                           | ((db+2) & 3))
            elif (dg+32) & 0xFF < 64 and ((dr-dg+8) & 0xFF) < 16 \
                    and ((db-dg+8) & 0xFF) < 16:
                out.append(0x80 | ((dg+32) & 63))
                out.append((((dr-dg+8) & 15) << 4) | ((db-dg+8) & 15))
            else: out += bytes((0xFE, p[0], p[1], p[2]))
        else: out += bytes((0xFF,) + p)
        idx[h(p)] = p; prev = p
    if run: out.append(0xC0 | (run - 1))
    open(path, "wb").write(bytes(out) + b"\0"*7 + b"\1")

def write_ppm(path, im):
    open(path, "wb").write(b"P6\n%d %d\n255\n" % (im.w, im.h) + im.rgb())

# ---- the scenes -----------------------------------------------------------
def scene_sunset(w=320, h=200):                       # -> SUNSET.JPG
    im = Img(w, h)
    horizon = int(h * 0.62)
    for y in range(h):
        if y < horizon:
            t = y / horizon
            c = (int(30+t*225), int(40+t*110), int(90+t*30))
        else:
            t = (y - horizon) / (h - horizon)
            c = (int(25+40*(1-t)), int(30+50*(1-t)), int(70+60*(1-t)))
        for x in range(w):
            im.set(x, y, c)
    im.disc(w*0.5, horizon-18, 26, (255, 214, 120))
    im.disc(w*0.5, horizon-18, 18, (255, 240, 190))
    for y in range(horizon, h, 3):                    # shimmer
        ww = max(4, 60 - (y - horizon))
        for x in range(int(w*0.5-ww/2), int(w*0.5+ww/2)):
            if (x + y) % 4 < 2: im.set(x, y, (255, 200, 130, 130))
    for i, (mx, mh) in enumerate([(0.16, 40), (0.28, 26), (0.83, 34)]):
        for y in range(horizon-mh, horizon):          # headlands
            t = (horizon - y) / mh
            half = int((1 - t) * w * 0.11)
            for x in range(int(mx*w)-half, int(mx*w)+half):
                im.set(x, y, (28+i*6, 30+i*6, 48+i*6))
    return im

def scene_bloom(w=256, h=256):                        # -> BLOOM.PNG (alpha)
    im = Img(w, h, (0, 0, 0, 0))
    cx = cy = w / 2
    for k in range(9):                                # petals
        ang = k * 2*math.pi/9
        px, py = cx + math.cos(ang)*58, cy + math.sin(ang)*58
        for t in range(100, 0, -2):
            r = 34 * t / 100
            c = (240, 120+int(90*t/100), 170, 235)
            im.disc(px, py, r, c)
    im.disc(cx, cy, 34, (255, 210, 80, 255))
    im.disc(cx, cy, 22, (200, 140, 40, 255))
    for k in range(24):                               # seeds
        a, r = k*2.4, 4 + k*0.7
        im.disc(cx + math.cos(a)*r, cy + math.sin(a)*r, 2, (120, 80, 30, 255))
    return im

def scene_flag(w=300, h=200):                         # -> FLAG.BMP
    im = Img(w, h)
    for y in range(h):
        for x in range(w):
            wave = math.sin(x/34.0 + y/60.0) * 8
            band = int((y + wave) / (h/4)) % 4
            c = [(0,0,170), (0,170,170), (170,0,170), (255,255,255)][band]
            im.set(x, y, c)
    im.disc(60, 60, 34, (255, 255, 255))
    im.disc(60, 60, 27, (0, 0, 170))
    im.disc(60, 60, 20, (255, 255, 255))              # the UnoDOS ring
    for y in range(28, 52):
        for x in range(56, 64): im.set(x, y, (0, 0, 170))
    return im

def scene_grad(w=256, h=160):                         # -> GRAD.TGA
    im = Img(w, h)
    for y in range(h):
        for x in range(w):
            im.set(x, y, (x*255//(w-1), y*255//(h-1),
                          255 - (x+y)*255//(w+h-2)))
    return im

def scene_moon(w=240, h=240):                         # -> MOON.PGM
    im = Img(w, h)
    im.disc(w/2, h/2, 84, (215, 215, 215))
    for cx, cy, r, g in [(96,86,17,150), (150,130,12,160), (110,150,22,140),
                         (140,74,8,170), (78,132,10,155), (160,166,9,150)]:
        im.disc(cx, cy, r, (g, g, g))
        im.disc(cx-2, cy-2, r*0.6, (g+25, g+25, g+25))
    return im

def scene_tiles(w=192, h=192):                        # -> TILES.QOI
    im = Img(w, h)
    for y in range(h):
        for x in range(w):
            t = ((x//24) + (y//24)) % 3
            c = [(30,30,110), (30,110,110), (110,30,110)][t]
            e = 8 if (x % 24 in (0, 23) or y % 24 in (0, 23)) else 0
            im.set(x, y, (min(255,c[0]+40+e*10), min(255,c[1]+40+e*10),
                          min(255,c[2]+40+e*10)) if e else c)
    return im

def scene_lagoon(w=320, h=200):                       # -> LAGOON.WEB (WebP)
    im = Img(w, h)
    horizon = int(h * 0.45)
    for y in range(h):                                # sky then sea
        if y < horizon:
            t = y / horizon
            im_c = (int(120+80*t), int(190+40*t), int(240-20*t))
        else:
            t = (y - horizon) / (h - horizon)
            im_c = (int(30+30*t), int(140-40*t), int(170-50*t))
        for x in range(w):
            im.set(x, y, im_c)
    for r in range(60, 10, -12):                      # lagoon rings
        im.disc(w*0.62, horizon+34, r, (60+r, 200-r, 200, 90))
    for y in range(horizon-16, horizon):              # island
        t = (horizon - y) / 16.0
        half = int((1-t) * 52)
        for x in range(int(w*0.28)-half, int(w*0.28)+half):
            im.set(x, y, (194, 178, 128))
    for k in range(5):                                # palm fronds
        ang = -0.6 - k*0.45
        for r in range(2, 30):
            im.set(int(w*0.28 + math.cos(ang)*r),
                   int(horizon-30 + math.sin(ang)*r*0.6), (40, 110, 50))
    for y in range(horizon-30, horizon-14):           # trunk
        im.set(int(w*0.28 + (y-(horizon-30))*0.25), y, (110, 80, 50))
        im.set(int(w*0.28 + (y-(horizon-30))*0.25)+1, y, (110, 80, 50))
    return im


def frames_orbit(n=10, w=120, h=120):                 # -> ORBIT.GIF
    fr = []
    for k in range(n):
        im = Img(w, h, (12, 14, 40, 255))
        im.disc(w/2, h/2, 14, (250, 205, 80))
        a = k * 2*math.pi/n
        im.disc(w/2 + math.cos(a)*40, h/2 + math.sin(a)*40, 8, (90, 170, 250))
        im.disc(w/2 + math.cos(a+2.2)*26, h/2 + math.sin(a+2.2)*26, 5,
                (230, 110, 110))
        fr.append(im)
    return fr

# ---- emit -----------------------------------------------------------------
def main():
    p = lambda n: os.path.join(OUT, n)
    write_png(p("BLOOM.PNG"), scene_bloom())
    write_bmp(p("FLAG.BMP"),  scene_flag())
    write_tga(p("GRAD.TGA"),  scene_grad())
    write_pgm(p("MOON.PGM"),  scene_moon())
    write_qoi(p("TILES.QOI"), scene_tiles())

    tmp = p("_sunset.ppm")
    write_ppm(tmp, scene_sunset())
    subprocess.run(["convert", tmp, "-quality", "90", p("SUNSET.JPG")],
                   check=True)
    os.remove(tmp)

    # WebP, named 8.3 (LAGOON.WEB) so the FAT lister shows it cleanly - the
    # decoder probes by RIFF magic, so the truncated extension is harmless
    tmp = p("_lagoon.ppm")
    write_ppm(tmp, scene_lagoon())
    subprocess.run(["convert", tmp, "-quality", "85", "webp:" + p("LAGOON.WEB")],
                   check=True)
    os.remove(tmp)

    fps = []
    for i, im in enumerate(frames_orbit()):
        fp = p("_orbit%02d.ppm" % i); write_ppm(fp, im); fps.append(fp)
    subprocess.run(["convert", "-delay", "8", "-loop", "0", *fps,
                    p("ORBIT.GIF")], check=True)
    for fp in fps: os.remove(fp)

    for f in sorted(os.listdir(OUT)):
        print(f, os.path.getsize(os.path.join(OUT, f)))

if __name__ == "__main__":
    main()
