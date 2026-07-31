#!/usr/bin/env python3
"""Generate sample Winamp 2 skins (.wsz) for UnoAmp.

A .wsz is a ZIP of BMP sprite sheets plus two text files. Real skins are drawn
by hand; these are generated, and they are built to be DIAGNOSTIC rather than
pretty: every sheet uses a distinct hue and every sprite cell is outlined, so
looking at the player tells you exactly which sheets loaded and whether the
sprite offsets in unoamp_ui.c line up with the format.

That is the point. A hand-drawn skin that renders wrong looks like bad art; one
of these renders wrong and you can see WHICH rectangle came from WHERE.

Sheet sizes are the classic ones - they are part of the format, not choices:

    MAIN 275x116   CBUTTONS 136x36   TITLEBAR 344x87   SHUFREP 92x85
    POSBAR 307x10  VOLUME 68x433     BALANCE 47x433    MONOSTER 56x24
    PLAYPAUS 42x9  NUMBERS 99x13     TEXT 155x18       EQMAIN 275x116
    PLEDIT 280x186

Two skins are written. One is DEFLATED and one is STORED, because ZIP method 0
and method 8 are different code paths in the loader and both turn up in the
wild.

    python3 tools/mkskin.py [outdir]
"""
import os, struct, sys, zipfile

# ---------------------------------------------------------------------------
# BMP writing. 24-bit, bottom-up, rows padded to 4 bytes - the plainest thing a
# BMP can be, because the loader's job is to read skins, not to be exercised on
# exotic encodings here.
# ---------------------------------------------------------------------------
def write_bmp(px, w, h):
    rowb = (w * 3 + 3) & ~3
    pad = b"\0" * (rowb - w * 3)
    rows = []
    for y in range(h - 1, -1, -1):          # bottom-up
        r = bytearray()
        for x in range(w):
            cr, cg, cb = px[y * w + x]
            r += bytes((cb, cg, cr))        # BMP stores B,G,R
        rows.append(bytes(r) + pad)
    data = b"".join(rows)
    hdr = b"BM" + struct.pack("<IHHI", 14 + 40 + len(data), 0, 0, 14 + 40)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    return hdr + data


class Sheet:
    def __init__(self, w, h, bg=(0, 0, 0)):
        self.w, self.h = w, h
        self.px = [bg] * (w * h)

    def rect(self, x, y, w, h, c):
        for j in range(max(0, y), min(self.h, y + h)):
            for i in range(max(0, x), min(self.w, x + w)):
                self.px[j * self.w + i] = c

    def frame(self, x, y, w, h, c):
        self.rect(x, y, w, 1, c); self.rect(x, y + h - 1, w, 1, c)
        self.rect(x, y, 1, h, c); self.rect(x + w - 1, y, 1, h, c)

    def cell(self, x, y, w, h, fill, edge):
        """A sprite cell: filled, outlined, with a corner pip. The pip is what
        makes a one-pixel offset error obvious - a misaligned sprite shows a
        pip in the wrong corner rather than looking merely 'a bit off'."""
        self.rect(x, y, w, h, fill)
        self.frame(x, y, w, h, edge)
        self.rect(x + 1, y + 1, 2, 2, edge)

    def bmp(self):
        return write_bmp(self.px, self.w, self.h)


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


# ---------------------------------------------------------------------------
# The 5x6 bitmap font. TEXT.BMP's character order is fixed by the skin format;
# a skin author draws INTO this layout, so it is reproduced exactly rather than
# invented. Glyphs are drawn from a tiny 3x5 stroke font, centred in the cell.
# ---------------------------------------------------------------------------
FONT_ROWS = [
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ\"@ ",
    "0123456789\x01:()-'!_+\\/[]^&%.=$#",
    "\x02\x03\x04?* ",
]

GLYPH = {
    "A": ("111", "101", "111", "101", "101"), "B": ("110", "101", "110", "101", "110"),
    "C": ("111", "100", "100", "100", "111"), "D": ("110", "101", "101", "101", "110"),
    "E": ("111", "100", "111", "100", "111"), "F": ("111", "100", "111", "100", "100"),
    "G": ("111", "100", "101", "101", "111"), "H": ("101", "101", "111", "101", "101"),
    "I": ("111", "010", "010", "010", "111"), "J": ("111", "001", "001", "101", "111"),
    "K": ("101", "101", "110", "101", "101"), "L": ("100", "100", "100", "100", "111"),
    "M": ("101", "111", "111", "101", "101"), "N": ("101", "111", "111", "111", "101"),
    "O": ("111", "101", "101", "101", "111"), "P": ("111", "101", "111", "100", "100"),
    "Q": ("111", "101", "101", "111", "011"), "R": ("111", "101", "110", "101", "101"),
    "S": ("111", "100", "111", "001", "111"), "T": ("111", "010", "010", "010", "010"),
    "U": ("101", "101", "101", "101", "111"), "V": ("101", "101", "101", "101", "010"),
    "W": ("101", "101", "111", "111", "101"), "X": ("101", "101", "010", "101", "101"),
    "Y": ("101", "101", "010", "010", "010"), "Z": ("111", "001", "010", "100", "111"),
    "0": ("111", "101", "101", "101", "111"), "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"), "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"), "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"), "7": ("111", "001", "001", "001", "001"),
    "8": ("111", "101", "111", "101", "111"), "9": ("111", "101", "111", "001", "111"),
    ".": ("000", "000", "000", "000", "010"), ":": ("000", "010", "000", "010", "000"),
    "-": ("000", "000", "111", "000", "000"), "(": ("001", "010", "010", "010", "001"),
    ")": ("100", "010", "010", "010", "100"), "'": ("010", "010", "000", "000", "000"),
    "!": ("010", "010", "010", "000", "010"), "_": ("000", "000", "000", "000", "111"),
    "+": ("000", "010", "111", "010", "000"), "/": ("001", "001", "010", "100", "100"),
    "\\": ("100", "100", "010", "001", "001"), "[": ("011", "010", "010", "010", "011"),
    "]": ("110", "010", "010", "010", "110"), "^": ("010", "101", "000", "000", "000"),
    "&": ("110", "110", "111", "101", "111"), "%": ("101", "001", "010", "100", "101"),
    "=": ("000", "111", "000", "111", "000"), "$": ("011", "110", "011", "110", "010"),
    "#": ("101", "111", "101", "111", "101"), "@": ("111", "101", "111", "100", "111"),
    '"': ("101", "101", "000", "000", "000"), "*": ("101", "010", "101", "000", "000"),
    "?": ("111", "001", "011", "000", "010"),
}


def draw_text_sheet(fg, bg):
    """TEXT.BMP: 3 rows of 5x6 cells. Glyphs are 3x5 at a 1px inset, which is
    how the real sheets sit - the sixth row and fifth column are the gap."""
    s = Sheet(155, 18, bg)
    for r, row in enumerate(FONT_ROWS):
        for i, ch in enumerate(row):
            g = GLYPH.get(ch)
            if not g:
                continue
            ox, oy = i * 5 + 1, r * 6
            for j, line in enumerate(g):
                for k, bit in enumerate(line):
                    if bit == "1":
                        s.px[(oy + j) * s.w + ox + k] = fg
    return s


def draw_numbers(fg, bg):
    """NUMBERS.BMP: ten 9x13 digits. Same stroke font, scaled 2x and centred,
    so the time display is legible rather than merely present."""
    s = Sheet(99, 13, bg)
    for d in range(10):
        g = GLYPH[str(d)]
        ox, oy = d * 9 + 1, 1
        for j, line in enumerate(g):
            for k, bit in enumerate(line):
                if bit == "1":
                    for jj in range(2):
                        for kk in range(2):
                            y, x = oy + j * 2 + jj, ox + k * 2 + kk
                            if 0 <= y < 13 and 0 <= x < 99:
                                s.px[y * s.w + x] = fg
    return s


# ---------------------------------------------------------------------------
# The skin
# ---------------------------------------------------------------------------
def build_skin(pal):
    bg, fg, hi, acc, edge = (pal["bg"], pal["fg"], pal["hi"], pal["acc"], pal["edge"])
    out = {}

    # MAIN: the 275x116 backdrop, with each functional region tinted so you can
    # see at a glance whether the controls land where the format says.
    m = Sheet(275, 116, bg)
    m.rect(0, 0, 275, 14, shade(hi, 0.8))            # titlebar strip
    m.rect(24, 26, 76, 16, shade(bg, 0.55))          # time well
    m.rect(107, 24, 160, 12, shade(bg, 0.55))        # title well
    m.rect(24, 43, 76, 16, shade(bg, 0.35))          # visualiser well
    m.frame(24, 43, 76, 16, edge)
    m.rect(16, 72, 248, 10, shade(bg, 0.7))          # posbar groove
    m.frame(0, 0, 275, 116, edge)
    out["MAIN.BMP"] = m

    # TITLEBAR: active strip at y=0, inactive at y=15, both 275 wide from x=27.
    t = Sheet(344, 87, bg)
    t.rect(27, 0, 275, 14, acc)
    t.rect(27, 15, 275, 14, shade(acc, 0.5))
    for x in range(27, 302, 6):                      # the classic hatch
        t.rect(x, 3, 3, 1, shade(acc, 1.4))
        t.rect(x, 18, 3, 1, shade(acc, 0.8))
    for i, (bx, by) in enumerate([(0, 0), (9, 0), (18, 0)]):
        t.cell(bx, by, 9, 9, hi, edge)               # menu / minimise / close
        t.cell(bx, by + 9, 9, 9, acc, edge)          # pressed
    out["TITLEBAR.BMP"] = t

    # CBUTTONS: six transport buttons, normal row then pressed row.
    widths = [23, 23, 23, 23, 22, 22]
    c = Sheet(136, 36, bg)
    x = 0
    for i, w in enumerate(widths):
        h = 16 if i == 5 else 18
        c.cell(x, 0, w, h, hi, edge)
        c.cell(x, 18 if i < 5 else 16, w, h, acc, edge)
        # a glyph per button so play/pause/stop are told apart at a glance
        cx, cy = x + w // 2, h // 2
        if i == 0: c.rect(cx - 4, cy - 3, 2, 7, fg); c.rect(cx, cy - 1, 4, 2, fg)
        if i == 1: c.rect(cx - 2, cy - 4, 2, 8, fg); c.rect(cx, cy - 2, 2, 4, fg)
        if i == 2: c.rect(cx - 3, cy - 4, 2, 8, fg); c.rect(cx + 1, cy - 4, 2, 8, fg)
        if i == 3: c.rect(cx - 3, cy - 3, 7, 7, fg)
        if i == 4: c.rect(cx + 2, cy - 3, 2, 7, fg); c.rect(cx - 4, cy - 1, 4, 2, fg)
        if i == 5: c.rect(cx - 4, cy + 1, 8, 2, fg); c.rect(cx - 1, cy - 4, 2, 4, fg)
        x += w
    out["CBUTTONS.BMP"] = c

    # SHUFREP: repeat (28 wide) and shuffle (47 wide), four states each at
    # y = 0/15/30/45; then the EQ and PL toggles at y = 61/73.
    s = Sheet(92, 85, bg)
    for i, y in enumerate((0, 15, 30, 45)):
        on = i >= 2
        s.cell(0, y, 28, 15, acc if on else hi, edge)
        s.cell(28, y, 47, 15, acc if on else hi, edge)
    for i, y in enumerate((61, 73)):
        s.cell(0, y, 23, 12, acc if i else hi, edge)     # EQ
        s.cell(23, y, 23, 12, acc if i else hi, edge)    # PL
        s.cell(46, y, 23, 12, shade(acc, 1.3), edge)     # pressed variants
        s.cell(69, y, 23, 12, shade(acc, 1.3), edge)
    out["SHUFREP.BMP"] = s

    # POSBAR: 248-wide groove, then the thumb normal (248) and pressed (278).
    p = Sheet(307, 10, bg)
    p.rect(0, 0, 248, 10, shade(bg, 0.7))
    p.frame(0, 0, 248, 10, edge)
    p.cell(248, 0, 29, 10, hi, edge)
    p.cell(278, 0, 29, 10, acc, edge)
    out["POSBAR.BMP"] = p

    # VOLUME / BALANCE: 28 frames stacked at 15px pitch, the bar growing with
    # the value so the slider position is readable without a thumb.
    for name, w in (("VOLUME.BMP", 68), ("BALANCE.BMP", 47)):
        v = Sheet(w, 433, bg)
        for i in range(28):
            y = i * 15
            v.rect(0, y, w, 13, shade(bg, 0.6))
            v.frame(0, y, w, 13, edge)
            lit = max(1, (w - 4) * i // 27)
            v.rect(2, y + 2, lit, 9, acc)
        out[name] = v

    # MONOSTER: stereo at x=0 (29 wide), mono at x=29 (27 wide); lit row y=0,
    # unlit row y=12.
    ms = Sheet(56, 24, bg)
    ms.cell(0, 0, 29, 12, acc, edge); ms.cell(0, 12, 29, 12, shade(bg, 0.7), edge)
    ms.cell(29, 0, 27, 12, acc, edge); ms.cell(29, 12, 27, 12, shade(bg, 0.7), edge)
    out["MONOSTER.BMP"] = ms

    # PLAYPAUS: play / pause / stop indicators, 9x9 each.
    pp = Sheet(42, 9, bg)
    pp.cell(0, 0, 9, 9, acc, edge)
    pp.cell(9, 0, 9, 9, hi, edge)
    pp.cell(18, 0, 9, 9, shade(bg, 0.7), edge)
    pp.rect(36, 0, 3, 9, acc); pp.rect(39, 0, 3, 9, hi)
    out["PLAYPAUS.BMP"] = pp

    out["NUMBERS.BMP"] = draw_numbers(acc, shade(bg, 0.4))
    out["TEXT.BMP"] = draw_text_sheet(fg, shade(bg, 0.5))

    # EQMAIN: the equaliser backdrop. The band wells are tinted so the sliders
    # unoamp_ui.c draws procedurally have something to sit in.
    e = Sheet(275, 116, bg)
    e.rect(0, 0, 275, 14, shade(hi, 0.8))
    e.rect(14, 18, 26, 12, hi); e.frame(14, 18, 26, 12, edge)    # the ON button
    e.rect(13, 30, 4, 20, edge)      # the two pixels eq_slider() borrows from
    e.rect(0, 34, 8, 8, hi)
    for i in range(11):
        x = 21 if i == 0 else 78 + (i - 1) * 18
        e.rect(x, 38, 12, 51, shade(bg, 0.55))
        e.frame(x, 38, 12, 51, edge)
    e.frame(0, 0, 275, 116, edge)
    out["EQMAIN.BMP"] = e

    # PLEDIT: the playlist nine-slice. Each piece is a different tint so a
    # mis-sliced corner is obvious rather than subtle.
    pl = Sheet(280, 186, bg)
    pl.cell(0, 0, 25, 20, acc, edge)                 # top-left
    pl.cell(26, 0, 100, 20, shade(acc, 1.2), edge)   # title
    pl.cell(127, 0, 25, 20, shade(acc, 0.8), edge)   # top tile
    pl.cell(153, 0, 25, 20, acc, edge)               # top-right
    pl.cell(0, 42, 12, 29, hi, edge)                 # left edge
    pl.cell(31, 42, 20, 29, hi, edge)                # right edge
    pl.cell(0, 72, 125, 38, shade(hi, 0.8), edge)    # bottom-left
    pl.cell(126, 72, 150, 38, shade(hi, 0.8), edge)  # bottom-right
    out["PLEDIT.BMP"] = pl
    return out


# viscolor.txt: 24 "r,g,b" lines. 0 background, 1 peak, 2..17 the bar gradient
# bottom to top, 18..23 the oscilloscope.
def viscolor(pal):
    lines = ["%d,%d,%d // %s" % (pal["bg"][0] // 3, pal["bg"][1] // 3, pal["bg"][2] // 3,
                                 "background"),
             "%d,%d,%d // peak" % pal["fg"]]
    lo, hi_ = pal["acc"], pal["hi"]
    for i in range(16):
        f = i / 15.0
        lines.append("%d,%d,%d" % tuple(int(lo[k] + (hi_[k] - lo[k]) * f) for k in range(3)))
    for i in range(6):
        lines.append("%d,%d,%d" % shade(pal["fg"], 0.5 + i * 0.1))
    return "\n".join(lines) + "\n"


def pledit_txt(pal):
    def hx(c):
        return "#%02X%02X%02X" % c
    return ("[Text]\nNormal=%s\nCurrent=%s\nNormalBG=%s\nSelectedBG=%s\n"
            % (hx(pal["fg"]), hx(pal["acc"]), hx(shade(pal["bg"], 0.4)), hx(pal["hi"])))


PALETTES = {
    # Warm dark, the classic "player at night" look.
    "Ember":  dict(bg=(38, 24, 20), fg=(255, 214, 170), hi=(150, 70, 40),
                   acc=(255, 140, 40), edge=(20, 12, 10)),
    # Light. Proves nothing in the renderer assumes a dark skin - a player that
    # only looks right on black is a player with a hardcoded colour somewhere.
    "Frost":  dict(bg=(226, 234, 242), fg=(20, 34, 54), hi=(150, 180, 210),
                   acc=(30, 110, 200), edge=(90, 110, 130)),
    # The Winamp house style: grey chassis, green readout.
    "Classic": dict(bg=(58, 58, 58), fg=(0, 255, 60), hi=(96, 96, 96),
                    acc=(0, 200, 50), edge=(24, 24, 24)),
    # Maximum contrast. If a sprite is one pixel out of place, this is the skin
    # that shows it - there is no shading to hide an edge in.
    "Mono":   dict(bg=(0, 0, 0), fg=(255, 255, 255), hi=(255, 255, 255),
                   acc=(255, 0, 255), edge=(255, 255, 0)),
    # Saturated, to catch a red/blue channel swap at a glance: this skin is
    # unmistakably purple-on-cyan, and byte-order bugs turn it green-on-orange.
    "Neon":   dict(bg=(18, 0, 36), fg=(0, 255, 220), hi=(90, 0, 140),
                   acc=(200, 0, 255), edge=(0, 255, 220)),
}

# Sheets deliberately left out of the PARTIAL skin below. Winamp falls back per
# sheet and so do we, so a skin missing artwork must lose that artwork and
# nothing else - which is only actually true if something tests it.
PARTIAL_DROP = ("VOLUME.BMP", "BALANCE.BMP", "NUMBERS.BMP", "PLEDIT.BMP")


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/skins"
    os.makedirs(outdir, exist_ok=True)
    jobs = [(n, p, None) for n, p in PALETTES.items()]
    # A skin with sheets missing, to exercise the per-sheet fallback.
    jobs.append(("Partial", PALETTES["Classic"], PARTIAL_DROP))
    for i, (name, pal, drop) in enumerate(jobs):
        sheets = build_skin(pal)
        if drop:
            for d in drop:
                sheets.pop(d, None)
        # Alternate deflate and store: both are legal .wsz and they are
        # different code paths in the loader, so shipping only one would leave
        # half the reader untested.
        mode = zipfile.ZIP_DEFLATED if i % 2 == 0 else zipfile.ZIP_STORED
        path = os.path.join(outdir, "%s.wsz" % name.upper()[:8])
        with zipfile.ZipFile(path, "w", mode) as z:
            for fn, sh in sheets.items():
                z.writestr(fn, sh.bmp())
            z.writestr("viscolor.txt", viscolor(pal))
            z.writestr("pledit.txt", pledit_txt(pal))
        print("%s  (%s, %d bytes)" %
              (path, "deflate" if mode == zipfile.ZIP_DEFLATED else "stored",
               os.path.getsize(path)))


if __name__ == "__main__":
    main()
