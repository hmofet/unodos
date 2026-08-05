#!/usr/bin/env python3
"""browser_cascade_urc - the CS3 real-page comparison: the same pages
rendered by the unoweb engine under BOTH cascades, screenshotted and
pixel-diffed. This is the evidence a default flip waits on.

Needs the ENGINE build (the cascade only styles pages the unoweb renderer
draws):

    cd pc64 && BROWSER_ENGINE=uw UNO_DEBUG=1 ./build.sh
    python3 tools/browser_cascade_urc.py

Writes shots/cascade_<page>_{builtin,libcss}.png per page + a summary of
differing-pixel percentages. Small diffs are expected only where the two
cascades genuinely disagree; large diffs are a bridge bug - read the shots.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

PAGES = ["uno:welcome", "uno:sample", "uno:script"]


def goto(ui, loc, settle=2.5):
    ui.key(ord('l'), ctrl=1)
    ui.key(0, scan=0x06, settle=0.05)             # End
    for _ in range(32):
        ui.key(8, settle=0.04)                    # clear the address field
    ui.text(loc)
    ui.key(13)
    time.sleep(settle)


def open_browser(ui):
    n = ui.app_count()
    for i in range(n - 1, -1, -1):
        try:
            ui.link.command("launch", i, timeout=15)
        except RuntimeError:
            continue
        time.sleep(3.0)
        if any(t.startswith("Browser") for t in ui.windows()):
            return i
        ui.link.command("close", timeout=10)
        time.sleep(0.6)
    raise SystemExit("no app slot opens the Browser")


def grab(ui, tag):
    return ui.shot(tag)


def diff_pngs(a, b):
    """differing-pixel fraction between two same-size PNGs (pure python:
    the ppm2png helper writes uncompressed-ish PNGs, so lean on png module-
    free reading via re-decoding with zlib)."""
    import struct, zlib

    def read_png(path):
        raw = open(path, "rb").read()
        pos = 8
        w = h = None
        idat = b""
        while pos < len(raw):
            ln, typ = struct.unpack(">I4s", raw[pos:pos + 8])
            data = raw[pos + 8:pos + 8 + ln]
            if typ == b"IHDR":
                w, h, depth, ctype = struct.unpack(">IIBB", data[:10])
                assert depth == 8 and ctype in (2, 6), "unexpected PNG shape"
                bpp = 3 if ctype == 2 else 4
            elif typ == b"IDAT":
                idat += data
            pos += 12 + ln
        px = zlib.decompress(idat)
        stride = w * bpp + 1
        rows = []
        prev = bytearray(w * bpp)
        for y in range(h):
            line = bytearray(px[y * stride + 1:(y + 1) * stride])
            f = px[y * stride]
            if f == 1:
                for i in range(bpp, len(line)):
                    line[i] = (line[i] + line[i - bpp]) & 255
            elif f == 2:
                for i in range(len(line)):
                    line[i] = (line[i] + prev[i]) & 255
            elif f == 3:
                for i in range(len(line)):
                    a_ = line[i - bpp] if i >= bpp else 0
                    line[i] = (line[i] + ((a_ + prev[i]) >> 1)) & 255
            elif f == 4:
                for i in range(len(line)):
                    a_ = line[i - bpp] if i >= bpp else 0
                    c_ = prev[i - bpp] if i >= bpp else 0
                    p = a_ + prev[i] - c_
                    pa, pb, pc = abs(p - a_), abs(p - prev[i]), abs(p - c_)
                    pr = a_ if (pa <= pb and pa <= pc) else (prev[i] if pb <= pc else c_)
                    line[i] = (line[i] + pr) & 255
            rows.append(bytes(line))
            prev = line
        return w, h, bpp, rows

    wa, ha, bpa, ra = read_png(a)
    wb, hb, bpb, rb = read_png(b)
    if (wa, ha, bpa) != (wb, hb, bpb):
        return 1.0
    # mask the shell chrome that legitimately changes between grabs: the
    # debug perf HUD (top rows) and the taskbar clock (bottom rows) - the
    # first run measured 0.46% "difference" that was entirely those two
    ndiff = 0
    top, bottom = 16, 22
    for y in range(top, ha - bottom):
        la, lb = ra[y], rb[y]
        for x in range(0, wa * bpa, bpa):
            if la[x:x + 3] != lb[x:x + 3]:
                ndiff += 1
    return ndiff / float(wa * max(1, ha - top - bottom))


def main():
    shots = {}
    with UrcUi() as ui:
        open_browser(ui)
        for cascade in ("builtin", "libcss"):
            goto(ui, "uno:engine/cascade/" + cascade, settle=1.5)
            for page in PAGES:
                goto(ui, page, settle=3.0)
                tag = "cascade_%s_%s" % (page.replace(":", "_"), cascade)
                shots.setdefault(page, {})[cascade] = grab(ui, tag)

    print()
    worst = 0.0
    for page in PAGES:
        frac = diff_pngs(shots[page]["builtin"], shots[page]["libcss"])
        worst = max(worst, frac)
        print("%-14s %6.2f%% pixels differ" % (page, frac * 100.0))
    print("\nworst page: %.2f%% - read the shots before judging" % (worst * 100.0))


if __name__ == "__main__":
    main()
