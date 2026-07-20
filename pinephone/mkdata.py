#!/usr/bin/env python3
"""Generate pinephone/build/gfx.s from the shared UnoDOS assets (PinePhone / AArch64).

The Pi port draws into the firmware-allocated **mailbox framebuffer** — a flat
640x480, 32bpp XRGB8888 surface (depth 32, allocated at boot via the VideoCore
property channel). So, exactly like the GBA Mode-3 port, there are no hardware
tiles: we plot an 8x8 bitmap font and 16x16 icons pixel by pixel, each pixel's
palette INDEX looked up in a 16-entry 32-bit palette held in RAM (so the Theme
app recolours by swapping the table + redrawing).

This emits GNU-as (aarch64-linux-gnu-as) data: the font, the icons (index bytes),
the Dostris piece tables, the music tune (note Hz for the PWM tone path), and the
theme palettes (32-bit XRGB).

Usage: python pinephone/mkdata.py   (from the repo root)
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
    for ch in row:
        paint.append(icmap[int(ch)])
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

# ---- Music: a tone tune; on the Pi the value is the note FREQUENCY in Hz, fed to
#      the PWM (headphone-jack) tone path. The same 'Ode to Joy' as every port. ----
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
music_song = [(NOTE_HZ[n], d) for n, d in TUNE]

# ---- Tracker: 32 rows x 4 channels, one byte per cell (0 empty, 1..8 = C4..C5).
#      The same demo riff shape as the C64/Genesis trackers, collapsed to the
#      single-voice note range; playback voice-steals into the one PCM voice. ----
TK_ROWS, TK_CHANS = 32, 4
TK_DEMO_ROWS = [
    # ch1 bass, ch2 arp, ch3 melody, ch4 accent (values 0=empty, 1..8=C4..C5)
    (1, 5, 0, 8), (0, 0, 0, 0), (0, 3, 0, 0), (0, 0, 0, 0),
    (1, 5, 8, 0), (0, 0, 0, 0), (0, 3, 5, 0), (0, 0, 0, 0),
    (5, 1, 8, 8), (0, 0, 0, 0), (0, 2, 0, 0), (0, 0, 0, 0),
    (5, 5, 6, 0), (0, 0, 0, 0), (0, 2, 0, 0), (0, 0, 0, 0),
    (4, 6, 6, 8), (0, 0, 0, 0), (0, 4, 0, 0), (0, 0, 0, 0),
    (4, 1, 5, 0), (0, 0, 0, 0), (0, 4, 3, 0), (0, 0, 0, 0),
    (5, 2, 6, 8), (0, 0, 0, 0), (0, 5, 0, 0), (0, 0, 0, 0),
    (5, 7, 8, 8), (0, 0, 0, 0), (0, 7, 0, 8), (0, 0, 0, 0)]
assert len(TK_DEMO_ROWS) == TK_ROWS
tk_demo = [v for row in TK_DEMO_ROWS for v in row]
# note frequencies 1..8 = C4..C5 (Hz), same scale as the Music tune
tk_freqs = [262, 294, 330, 349, 392, 440, 494, 523]

# ---- Pac-Man maze template (28x25) - byte-identical to the Genesis/Amiga port:
#      0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate --------------------------
PM_MAZE = """
1111111111111111111111111111
1322222222222112222222222231
1211121111112112111111211121
1211121111112112111111211121
1222222222222222222222222221
1211121211111111111121211121
1222221222212222122221222221
1111121111010000101111211111
0000121000000000000001210000
0000121011115005111101210000
1111121014444444444101211111
0000020014444444444100200000
0000121014444444444101210000
0000121011111111111101210000
1111121000000000000001211111
1222222222212222122222222221
1211121111212112121111211121
1321122222222222222222211231
1121121211111111111121211211
1222221222212222122221222221
1211111111212112121111111121
1222222222222222222222222221
1211121111112112111111211121
1222222222222112222222222221
1111111111111111111111111111
"""
pm_maze = [int(ch) for ln in PM_MAZE.split() for ch in ln]
assert len(pm_maze) == 28 * 25, len(pm_maze)

# ---- palettes: 4 theme presets x 16 colours (32-bit XRGB 0xFFRRGGBB) ------------
def c(r, g, b):
    return (0xFF << 24) | (r << 16) | (g << 8) | b
#       0 desktop      1 white         2 cyan          3 magenta       4 black
BASE = [None,          c(255,255,255), c(0,230,230),   c(230,0,230),   c(0,0,0),
#       5 green        6 yellow        7 red           8 orange        9 grey
        c(0,220,0),    c(255,235,0),   c(235,0,0),     c(255,140,0),   c(128,128,128)]
BASE += [c(0,0,0)] * (16 - len(BASE))
NTHEMES = 4
DESKTOPS = [c(32,64,160), c(16,96,40), c(150,24,24), c(56,56,72)]   # blue green red grey
THEMES = []
for dk in DESKTOPS:
    pal = list(BASE)
    pal[0] = dk
    THEMES.append(pal)

# ---- assemble-time constants (kernel.s .includes these) -------------------------
EQU_OUT = os.path.join(HERE, "build", "gfxequ.inc")
with open(EQU_OUT, "w", newline="\n") as f:
    f.write("// AUTO-GENERATED by pinephone/mkdata.py - do not edit\n")
    f.write(".equ NICONS, %d\n" % NICONS)
    f.write(".equ NTHEMES, %d\n" % NTHEMES)
    f.write(".equ MUSIC_COUNT, %d\n" % len(music_song))
    f.write(".equ TK_ROWS, %d\n" % TK_ROWS)
    f.write(".equ TK_CHANS, %d\n" % TK_CHANS)

# ---- emit -----------------------------------------------------------------------
with open(OUT, "w", newline="\n") as f:
    f.write("// AUTO-GENERATED by pinephone/mkdata.py - do not edit\n")
    f.write(".section .rodata\n.align 3\n")
    # font: 95*8 bytes
    f.write(".global font_data\nfont_data:\n")
    for g in range(95):
        f.write("    .byte " + ",".join(str(b) for b in font[g*8:(g+1)*8]) + "\n")
    # icons: NICONS * 256 index bytes
    f.write(".global icon_data\nicon_data:\n")
    for name, idxs in zip([n for n, _ in ICONS] + ["paint"], icons):
        f.write(f"    // {name}\n")
        for r in range(16):
            f.write("    .byte " + ",".join(str(v) for v in idxs[r*16:(r+1)*16]) + "\n")
    # piece tables
    f.write(".global piece_masks\n.align 1\npiece_masks:\n")
    for i, p in enumerate(order):
        f.write("    .hword " + ",".join("0x%04X" % piece_masks[i*4+r] for r in range(4)) + "\n")
    f.write(".global piece_tiles\npiece_tiles:\n    .byte " +
            ",".join(str(t) for t in piece_tiles) + "\n")
    # music: freq (hword), frames (byte), pad (byte) -> 4 bytes per note
    f.write(".global music_song\n.align 2\nmusic_song:\n")
    for hz, fr in music_song:
        f.write(f"    .hword {hz}\n    .byte {fr}\n    .byte 0\n")
    # theme palettes: NTHEMES * 16 XRGB words
    f.write(".global theme_pals\n.align 2\ntheme_pals:\n")
    for ti, pal in enumerate(THEMES):
        f.write(f"    // theme {ti}\n    .word " + ",".join("0x%08X" % v for v in pal) + "\n")
    # tracker: demo pattern (32x4 bytes) + note frequency table (8 hwords)
    f.write(".global tk_demo\ntk_demo:\n")
    for r in range(TK_ROWS):
        f.write("    .byte " + ",".join(str(v) for v in tk_demo[r*TK_CHANS:(r+1)*TK_CHANS]) + "\n")
    f.write(".global tk_freqs\n.align 1\ntk_freqs:\n    .hword " +
            ",".join(str(v) for v in tk_freqs) + "\n")
    # pac-man maze template (28x25 bytes)
    f.write(".global pm_maze_tpl\npm_maze_tpl:\n")
    for r in range(25):
        f.write("    .byte " + ",".join(str(v) for v in pm_maze[r*28:(r+1)*28]) + "\n")

print(f"wrote {OUT}: font 95, {NICONS} icons, {len(music_song)} notes, {NTHEMES} themes, "
      f"tracker {TK_ROWS}x{TK_CHANS}, pacman maze 28x25")
