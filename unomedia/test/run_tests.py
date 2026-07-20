#!/usr/bin/env python3
"""unomedia decoder test runner (host).

Builds imgtest (hosted gcc over the same sources the .UNO module compiles
freestanding), generates reference images with ImageMagick `convert` plus a
few hand-crafted files, decodes each with imgtest, and compares the RGBA
output against ImageMagick's own decode of the same file.

  lossless formats  -> pixel-exact (max channel delta 0)
  JPEG              -> PSNR >= 40 dB vs ImageMagick's decode (both follow
                       the spec; only rounding differs)
  animated GIF      -> per-frame vs `convert -coalesce`
  refusal cases     -> expect ERR: with the right reason

Usage: python3 run_tests.py [filter-substring]
"""
import os, subprocess, sys, struct, zlib, math, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
UM   = os.path.dirname(HERE)
GEN  = os.path.join(HERE, "gen")
BIN  = os.path.join(GEN, "imgtest")

CONVERT = shutil.which("convert") or "convert"

def sh(*args, **kw):
    return subprocess.run(list(args), check=True, capture_output=True, **kw)

def build():
    os.makedirs(GEN, exist_ok=True)
    srcs = [os.path.join(UM, f) for f in sorted(os.listdir(UM))
            if f.endswith(".c")]
    srcs.append(os.path.join(HERE, "imgtest.c"))
    sh("gcc", "-O2", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra",
       "-I", UM, "-o", BIN, *srcs, "-lm")

def ref_rgba(path, coalesce=False):
    """ImageMagick's decode of `path` as raw RGBA (all frames)."""
    args = [CONVERT, path]
    if coalesce: args += ["-coalesce"]
    args += ["-depth", "8", "RGBA:-"]
    return sh(*args).stdout

def psnr(a, b):
    if len(a) != len(b) or not a: return 0.0
    se = 0
    for x, y in zip(a, b):
        d = x - y; se += d * d
    if se == 0: return 99.0
    return 10.0 * math.log10(255.0 * 255.0 * len(a) / se)

def decode(path, maxfr=64):
    out = os.path.join(GEN, "out.raw")
    r = subprocess.run([BIN, "decode", path, out, str(maxfr)],
                       capture_output=True, text=True)
    raw = open(out, "rb").read() if os.path.exists(out) else b""
    return r.returncode, r.stdout.strip(), raw

# ---- hand-crafted generators (formats convert can't emit here) -------------
def write_qoi(path, w, h, px):        # px = list of (r,g,b,a), QOI spec 1.0
    out = bytearray(b"qoif" + struct.pack(">IIBB", w, h, 4, 0))
    idx = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    def hidx(p): return (p[0]*3 + p[1]*5 + p[2]*7 + p[3]*11) % 64
    for p in px:
        if p == prev:
            run += 1
            if run == 62: out.append(0xC0 | (run - 1)); run = 0
            continue
        if run: out.append(0xC0 | (run - 1)); run = 0
        if idx[hidx(p)] == p:
            out.append(hidx(p))
        elif p[3] == prev[3]:
            dr = (p[0]-prev[0]+256) % 256; dg = (p[1]-prev[1]+256) % 256
            db = (p[2]-prev[2]+256) % 256
            sdr = ((dr+2) % 256); sdg = ((dg+2) % 256); sdb = ((db+2) % 256)
            if sdr < 4 and sdg < 4 and sdb < 4:
                out.append(0x40 | (sdr << 4) | (sdg << 2) | sdb)
            else:
                vg  = (dg + 32) % 256
                vgr = (dr - dg + 8 + 256) % 256
                vgb = (db - dg + 8 + 256) % 256
                if vg < 64 and vgr < 16 and vgb < 16:
                    out.append(0x80 | vg); out.append((vgr << 4) | vgb)
                else:
                    out += bytes((0xFE, p[0], p[1], p[2]))
        else:
            out += bytes((0xFF, p[0], p[1], p[2], p[3]))
        idx[hidx(p)] = p
        prev = p
    if run: out.append(0xC0 | (run - 1))
    out += b"\0\0\0\0\0\0\0\1"
    open(path, "wb").write(out)

def gradient_px(w, h):
    px = []
    for y in range(h):
        for x in range(w):
            px.append(((x * 255) // max(w - 1, 1),
                       (y * 255) // max(h - 1, 1),
                       ((x + y) * 255) // max(w + h - 2, 1), 255))
    return px

def raw_rgba(px):
    return b"".join(bytes(p) for p in px)

# ---- test table ------------------------------------------------------------
PASS = []; FAIL = []

def check(name, ok, why=""):
    (PASS if ok else FAIL).append(name)
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + why))

def cmp_exact(name, path, coalesce=False, maxfr=64):
    rc, head, raw = decode(path, maxfr)
    if rc != 0: check(name, False, head); return
    ref = ref_rgba(path, coalesce)
    if len(raw) != len(ref):
        check(name, False, f"size {len(raw)} != ref {len(ref)} ({head})"); return
    delta = max((abs(a - b) for a, b in zip(raw, ref)), default=0)
    check(name, delta == 0, f"max delta {delta} ({head})")

def cmp_psnr(name, path, mindb=40.0):
    rc, head, raw = decode(path)
    if rc != 0: check(name, False, head); return
    ref = ref_rgba(path)
    if len(raw) != len(ref):
        check(name, False, f"size {len(raw)} != ref {len(ref)} ({head})"); return
    db = psnr(raw, ref)
    check(name, db >= mindb, f"PSNR {db:.1f} dB < {mindb} ({head})")

def expect_err(name, path, needle):
    rc, head, _ = decode(path)
    ok = rc != 0 and needle.lower() in head.lower()
    check(name, ok, f"got: {head}")

def main():
    filt = sys.argv[1] if len(sys.argv) > 1 else ""
    build()
    os.makedirs(GEN, exist_ok=True)
    g = lambda n: os.path.join(GEN, n)
    base = g("base.png")                       # the shared source scene
    sh(CONVERT, "-size", "97x61", "gradient:red-blue",
       "(", "-size", "40x30", "xc:rgba(0,255,0,0.5)", ")",
       "-geometry", "+20+10", "-composite", base)

    # ---- PNG ----
    for ct, name in [("", "png_rgba"),
                     ("-alpha off", "png_rgb"),
                     ("-colorspace Gray -alpha off", "png_gray"),
                     ("-colors 100 -type Palette -alpha off", "png_pal")]:
        p = g(name + ".png")
        sh(*([CONVERT, base] + (ct.split() if ct else []) + [p]))
        if filt in name: cmp_exact(name, p)
    p = g("png_16bit.png"); sh(CONVERT, base, "-depth", "16", p)
    if filt in "png_16bit": cmp_psnr("png_16bit", p, 45.0)   # we fold 16->8
    p = g("png_interlace.png"); sh(CONVERT, base, "-interlace", "PNG", p)
    if filt in "png_interlace": cmp_exact("png_interlace", p)
    p = g("png_1bit.png")
    sh(CONVERT, base, "-alpha", "off", "-monochrome", p)
    if filt in "png_1bit": cmp_exact("png_1bit", p)

    # ---- JPEG ----
    for q, samp, name in [("92", "4:2:0", "jpg_q92_420"),
                          ("75", "4:4:4", "jpg_q75_444"),
                          ("85", "4:2:2", "jpg_q85_422")]:
        p = g(name + ".jpg")
        sh(CONVERT, base, "-quality", q, "-sampling-factor", samp, p)
        if filt in name: cmp_psnr(name, p)
    p = g("jpg_gray.jpg")
    sh(CONVERT, base, "-colorspace", "Gray", "-quality", "90", p)
    if filt in "jpg_gray": cmp_psnr("jpg_gray", p)
    p = g("jpg_prog.jpg")
    sh(CONVERT, base, "-interlace", "Plane", "-quality", "85", p)
    if filt in "jpg_prog": cmp_psnr("jpg_prog", p)
    p = g("jpg_prog_gray.jpg")
    sh(CONVERT, base, "-colorspace", "Gray", "-interlace", "Plane",
       "-quality", "90", p)
    if filt in "jpg_prog_gray": cmp_psnr("jpg_prog_gray", p)
    p = g("jpg_prog_420.jpg")
    sh(CONVERT, base, "-interlace", "Plane", "-sampling-factor", "4:2:0",
       "-quality", "80", p)
    if filt in "jpg_prog_420": cmp_psnr("jpg_prog_420", p)

    # ---- GIF ----
    p = g("gif_still.gif"); sh(CONVERT, base, "+dither", "-colors", "64", p)
    if filt in "gif_still": cmp_exact("gif_still", p)
    p = g("gif_anim.gif")
    sh(CONVERT, "-delay", "10", "-size", "40x30",
       "xc:red", "xc:green", "xc:blue", "-loop", "0", p)
    if filt in "gif_anim": cmp_exact("gif_anim", p, coalesce=True)

    # ---- BMP ----
    for extra, name in [([], "bmp_24"),
                        (["-define", "bmp:format=bmp4"], "bmp_v4"),
                        (["-colors", "16", "-type", "Palette",
                          "-alpha", "off", "-compress", "RLE"], "bmp_rle8")]:
        p = g(name + ".bmp")
        sh(*([CONVERT, base, "-alpha", "off"] + extra + [p]))
        if filt in name: cmp_exact(name, p)

    # ---- TGA / PNM / ICO ----
    p = g("tga_rle.tga"); sh(CONVERT, base, "-compress", "RLE", p)
    if filt in "tga_rle": cmp_exact("tga_rle", p)
    p = g("tga_raw.tga"); sh(CONVERT, base, "-compress", "None", p)
    if filt in "tga_raw": cmp_exact("tga_raw", p)
    p = g("pnm_p6.ppm"); sh(CONVERT, base, "-alpha", "off", p)
    if filt in "pnm_p6": cmp_exact("pnm_p6", p)
    p = g("pnm_p5.pgm")
    sh(CONVERT, base, "-colorspace", "Gray", "-alpha", "off", p)
    if filt in "pnm_p5": cmp_exact("pnm_p5", p)
    p = g("ico_32.ico"); sh(CONVERT, base, "-resize", "32x32!", p)
    if filt in "ico_32": cmp_exact("ico_32", p)

    # ---- QOI (hand-built; compared against the pixels we encoded) ----
    if filt in "qoi_grad":
        w, h = 33, 21
        px = gradient_px(w, h)
        write_qoi(g("qoi_grad.qoi"), w, h, px)
        rc, head, raw = decode(g("qoi_grad.qoi"))
        check("qoi_grad", rc == 0 and raw == raw_rgba(px),
              head if rc else "pixel mismatch")

    # ---- WebP ----
    # lossy: both decoders read the same deterministic VP8 bitstream, but
    # chroma upsampling + YUV->RGB rounding differ, hence PSNR not exact
    p = g("webp_lossy.webp")
    sh("ffmpeg", "-v", "quiet", "-y", "-i", base,
       "-c:v", "libwebp", "-quality", "80", p)
    if filt in "webp_lossy": cmp_psnr("webp_lossy", p, 35.0)
    p = g("webp_lossless.webp")
    sh("ffmpeg", "-v", "quiet", "-y", "-i", base,
       "-c:v", "libwebp", "-lossless", "1", p)
    if filt in "webp_lossless": cmp_exact("webp_lossless", p)
    p = g("webp_alpha.webp")     # lossless keeps the base scene's real alpha
    sh("ffmpeg", "-v", "quiet", "-y", "-i", base, "-pix_fmt", "rgba",
       "-c:v", "libwebp", "-lossless", "1", p)
    if filt in "webp_alpha": cmp_exact("webp_alpha", p)
    p = g("webp_lossy_alpha.webp")   # ALPH chunk on a lossy frame
    sh("ffmpeg", "-v", "quiet", "-y", "-i", base, "-pix_fmt", "yuva420p",
       "-c:v", "libwebp", "-quality", "80", p)
    if filt in "webp_lossy_alpha": cmp_psnr("webp_lossy_alpha", p, 35.0)
    p = g("webp_anim.webp")
    sh("ffmpeg", "-v", "quiet", "-y", "-f", "lavfi",
       "-i", "testsrc=size=64x48:rate=5:duration=1",
       "-c:v", "libwebp_anim", "-lossless", "1", "-loop", "0", p)
    if filt in "webp_anim": cmp_exact("webp_anim", p, coalesce=True)

    # ---- refusals ----
    if filt in "trunc_png":
        data = open(base, "rb").read()
        open(g("trunc.png"), "wb").write(data[:len(data) // 3])
        rc, head, _ = decode(g("trunc.png"))
        check("trunc_png_clean_fail", rc != 0, f"got: {head}")

    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    if FAIL: sys.exit(1)

if __name__ == "__main__":
    main()
