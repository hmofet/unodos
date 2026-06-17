#!/usr/bin/env python3
"""Generate gba/build/gfx.s from the shared UnoDOS assets (Game Boy Advance).

The GBA port draws into a flat **Mode 3** framebuffer (240x160, 16bpp BGR555),
so there are no hardware tiles — we plot an 8x8 bitmap font and 16x16 icons pixel
by pixel, looking each pixel's palette INDEX up in a 16-entry BGR555 palette held
in IWRAM (so the Theme app can recolour by swapping the table + redrawing). This
emits GNU-as (`arm-none-eabi-as`) data: the font, the icons (as index bytes), the
Dostris piece tables, the APU tune, and the theme palettes.

Usage: python gba/mkdata.py   (from the repo root)
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(HERE, "build", "gfx.s")

# ---- shared 8x8 font (1bpp; the same kernel/font8x8.asm every port uses) --------
font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

# ---- icons from the x86 .BIN donors -> 16x16 palette-index maps -----------------
# donor chunky 0-3 -> palette index: 0->0 desktop, 1->2 cyan, 2->3 magenta, 3->1 white
icmap = {0: 0, 1: 2, 2: 3, 3: 1}
def icon_idx(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    out = []
    for r in range(16):
        rowb = chunky[r * 4:(r + 1) * 4]
        for px in range(16):
            out.append(icmap[(rowb[px // 4] >> ((3 - (px % 4)) * 2)) & 3])
    return out

ICONS = [("sysinfo", "build/sysinfo.bin"), ("clock", "build/clock.bin"),
         ("notepad", "build/notepad.bin"), ("music", "build/music.bin"),
         ("files", "build/browser.bin"), ("theme", "build/settings.bin"),
         ("tracker", "build/tracker.bin"), ("dostris", "build/tetris.bin"),
         ("outlast", "build/outlast.bin"), ("pacman", "build/pacman.bin")]
NICONS = len(ICONS) + 1
PAINT_ICON = [
    "0000000000033000", "0000000000333000", "0000000003330000", "0000000033300000",
    "0000000333000000", "0000011330000000", "0000111100000000", "0000111000000000",
    "0001110000000000", "0001100000000000", "0022000022220000", "0222200222222000",
    "2222222222222200", "2222222222222220", "0222222222222200", "0002222222220000"]
icons = []
for name, binfile in ICONS:
    p = os.path.join(ROOT, binfile)
    if not os.path.exists(p):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    icons.append(icon_idx(p))
paint = []
for row in PAINT_ICON:
    for c in row:
        paint.append(icmap[int(c)])
icons.append(paint)

# ---- Dostris tetromino tables (same Contract shape as every port) ---------------
PIECES = {
    "I": ["....", "####", "....", "...."], "O": [".##.", ".##.", "....", "...."],
    "T": [".#..", "###.", "....", "...."], "S": [".##.", "##..", "....", "...."],
    "Z": ["##..", ".##.", "....", "...."], "J": ["#...", "###.", "....", "...."],
    "L": ["..#.", "###.", "....", "...."]}
def rot90(g):
    return ["".join(g[3 - c][r] for c in range(4)) for r in range(4)]
def mask(g):
    m = 0
    for r in range(4):
        for c in range(4):
            if g[r][c] == "#":
                m |= 1 << (15 - (r * 4 + c))
    return m
order = ["I", "O", "T", "S", "Z", "J", "L"]
piece_masks = []
for p in order:
    g = PIECES[p]
    for _ in range(4):
        piece_masks.append(mask(g))
        g = rot90(g)
# piece -> palette index (block colour): I=cyan O=yellow T=magenta S=green Z=red J=white L=orange
piece_tiles = [2, 6, 3, 5, 7, 1, 8]

# ---- Music: a tone tune (GBA square channel 1, GB-style 11-bit period) ----------
NOTE_HZ = {"C4": 262, "D4": 294, "E4": 330, "F4": 349, "G4": 392, "A4": 440,
           "B4": 494, "C5": 523, "D5": 587}
Q, H = 26, 52
TUNE = [
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("E4", H), ("D4", Q), ("D4", H),
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("D4", H), ("C4", Q), ("C4", H)]
music_song = [(round(2048 - 131072 / NOTE_HZ[n]) & 0x7FF, d) for n, d in TUNE]

# ---- palettes: 4 theme presets x 16 colours (BGR555) ----------------------------
def c(r, g, b):
    return (b << 10) | (g << 5) | r
#       0 desktop      1 white  2 cyan      3 magenta   4 black 5 green    6 yellow    7 red       8 orange    9 grey
BASE = [None,          c(31,31,31), c(0,28,28), c(28,0,28), c(0,0,0), c(0,31,0), c(31,31,0), c(31,0,0), c(31,16,0), c(16,16,16)]
BASE += [c(0,0,0)] * (16 - len(BASE))
NTHEMES = 4
DESKTOPS = [c(4, 8, 24), c(2, 16, 4), c(20, 2, 2), c(6, 6, 8)]   # blue green red grey
THEMES = []
for dk in DESKTOPS:
    pal = list(BASE)
    pal[0] = dk
    THEMES.append(pal)

# ---- emit the .equ constants separately (kernel.s .includes these so they are
#      known at assemble time; the data labels below are resolved at link time) ---
EQU_OUT = os.path.join(HERE, "build", "gfxequ.inc")
with open(EQU_OUT, "w", newline="\n") as f:
    f.write("@ AUTO-GENERATED by gba/mkdata.py - do not edit\n")
    f.write(".equ NICONS, %d\n" % NICONS)
    f.write(".equ NTHEMES, %d\n" % NTHEMES)
    f.write(".equ MUSIC_COUNT, %d\n" % len(music_song))

# ---- emit -----------------------------------------------------------------------
with open(OUT, "w", newline="\n") as f:
    f.write("@ AUTO-GENERATED by gba/mkdata.py - do not edit\n")
    f.write(".section .rodata\n.align 2\n")
    # font: 95*8 bytes
    f.write(".global font_data\nfont_data:\n")
    for g in range(95):
        f.write("    .byte " + ",".join(str(b) for b in font[g*8:(g+1)*8]) + "\n")
    # icons: NICONS * 256 index bytes
    f.write(".global icon_data\nicon_data:\n")
    for name, idxs in zip([n for n, _ in ICONS] + ["paint"], icons):
        f.write(f"    @ {name}\n")
        for r in range(16):
            f.write("    .byte " + ",".join(str(v) for v in idxs[r*16:(r+1)*16]) + "\n")
    # piece tables
    f.write(".global piece_masks\n.align 2\npiece_masks:\n")
    for i, p in enumerate(order):
        f.write("    .hword " + ",".join("0x%04X" % piece_masks[i*4+r] for r in range(4)) + "\n")
    f.write(".global piece_tiles\npiece_tiles:\n    .byte " +
            ",".join(str(t) for t in piece_tiles) + "\n")
    # music: period (hword), frames (byte) per note -> emit as struct of 4 bytes (pad)
    f.write(".global music_song\n.align 2\nmusic_song:\n")
    for x, fr in music_song:
        f.write(f"    .hword 0x{x:04X}\n    .byte {fr}\n    .byte 0\n")
    # theme palettes: NTHEMES * 16 BGR555 hwords
    f.write(".global theme_pals\n.align 2\ntheme_pals:\n")
    for ti, pal in enumerate(THEMES):
        f.write(f"    @ theme {ti}\n    .hword " + ",".join("0x%04X" % v for v in pal) + "\n")

print(f"wrote {OUT}: font 95, {NICONS} icons, {len(music_song)} notes, {NTHEMES} themes")
