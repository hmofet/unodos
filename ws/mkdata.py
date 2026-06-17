#!/usr/bin/env python3
"""Generate ws/build/ws_data.inc + ws/build/ws_equ.inc (Bandai WonderSwan).

The WonderSwan is a hardware-tile machine (8x8, 2bpp planar), so unlike the GBA's
software framebuffer this bakes real tile PATTERNS the kernel DMA-copies into the
internal RAM tile area (0x2000). Tile index == ASCII for the font (so the kernel
just writes a char's code into the SCR1 map), plus a solid block tile and a set of
8x8 launcher icons. Colour comes from the per-tile palette field, so the Theme app
recolours everything by rewriting the mono shade-pool (ports 0x1C-0x1F) — no tile
data changes. Also emits the shared Dostris piece tables and the tone tune.

Usage: python ws/mkdata.py   (from the repo root or ws/)
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
os.makedirs(os.path.join(HERE, "build"), exist_ok=True)

# ---- shared 8x8 font (1bpp; the same kernel/font8x8.asm every port uses) ---------
font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, "font rows: %d" % len(font)   # ASCII 32..126

# ---- hand-drawn 8x8 launcher icons (1bpp, MSB = leftmost pixel) ------------------
def ic(*rows):
    return [int(r.replace(".", "0").replace("#", "1"), 2) for r in rows]
ICONS = [
    ("sysinfo", ic("..####..", ".#....#.", "#..##..#", "#.#..#.#", "#.#..#.#", "#..##..#", ".#....#.", "..####..")),
    ("clock",   ic("..####..", ".#....#.", "#...#..#", "#...#..#", "#...###.", "#......#", ".#....#.", "..####..")),
    ("notepad", ic("########", "#......#", "#.####.#", "#......#", "#.####.#", "#......#", "#.####.#", "########")),
    ("files",   ic("........", ".###....", "#####...", "#......#", "#......#", "#......#", "########", "........")),
    ("theme",   ic("...##...", "..#..#..", ".#....#.", "#..##..#", "#.####.#", ".#....#.", "..#..#..", "...##...")),
    ("music",   ic("...####.", "...#..#.", "...#..#.", "...#..#.", ".###..#.", "####.###", ".##..##.", "........")),
    ("dostris", ic("........", ".######.", ".######.", "...##...", "...##...", ".######.", ".######.", "........")),
    ("tracker", ic("#.#.#.#.", "#.#.#.#.", "#.#.#.#.", "###.#.#.", "#.###.#.", "#.#.###.", "#.#.#.##", "#.#.#.#.")),
    ("outlast", ic("..####..", ".######.", "#.#..#.#", "########", "########", "#.#..#.#", "##.##.##", "#.#..#.#")),
    ("pacman",  ic("..####..", ".###....", "##.#....", "###.....", "####....", "###.....", ".####...", "..####..")),
    ("paint",   ic(".....###", "....###.", "...###..", "..###...", ".###....", "###.....", "##......", "#.......")),
]
NICONS = len(ICONS)

# ---- 2bpp planar tile encoder: a 1bpp glyph -> 16 bytes (plane0=glyph, plane1=0) -
def tile2bpp(glyph8):
    out = []
    for b in glyph8:
        out.append(b & 0xFF)   # plane 0 (colour bit 0) = the glyph
        out.append(0x00)       # plane 1
    return out

TILE_BLOCK = 128               # solid-fill tile (recoloured per piece by its palette)
ICON_BASE = 129                # first launcher-icon tile
NTILES = ICON_BASE + NICONS

tiles = []
for t in range(NTILES):
    if 32 <= t <= 126:
        tiles.append(tile2bpp(font[(t - 32) * 8:(t - 32) * 8 + 8]))
    elif t == TILE_BLOCK:
        tiles.append(tile2bpp([0xFF] * 8))             # solid
    elif ICON_BASE <= t < ICON_BASE + NICONS:
        tiles.append(tile2bpp(ICONS[t - ICON_BASE][1]))
    else:
        tiles.append([0] * 16)                         # blank

# ---- Dostris tetromino tables (same Contract shape as every port) ----------------
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
# piece -> palette index (2..7) giving each tetromino a distinct mono shade
piece_pal = [2, 3, 4, 5, 6, 7, 2]

# ---- Music: a tone tune (WonderSwan sound channel 1, 11-bit period) --------------
# WS ch1 period reg = 2048 - (3072000 / (32 * freq))  (32-sample wavetable)
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
music_song = [(round(2048 - 96000 / NOTE_HZ[n]) & 0x7FF, d) for n, d in TUNE]

# ---- Theme presets: each is the 4 shade-pool bytes (ports 0x1C-0x1F) -------------
# pool[0]=white(0)..pool[7]=black(15); a theme shifts the whole ramp (tint/contrast).
def pool(p0, p1, p2, p3, p4, p5, p6, p7):
    return [(p1 << 4) | p0, (p3 << 4) | p2, (p5 << 4) | p4, (p7 << 4) | p6]
THEMES = [
    pool(0, 2, 4, 6, 9, 11, 13, 15),    # full-range greyscale (default)
    pool(0, 1, 2, 3, 4, 5, 6, 8),       # soft / low-contrast
    pool(2, 4, 6, 8, 10, 12, 14, 15),   # dark / high ink
    pool(15, 13, 11, 9, 6, 4, 2, 0),    # inverted (light on dark)
]
NTHEMES = len(THEMES)

# ---- emit the assemble-time equates (kernel.asm %includes this first) ------------
with open(os.path.join(HERE, "build", "ws_equ.inc"), "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by ws/mkdata.py - do not edit\n")
    f.write("NICONS       equ %d\n" % NICONS)
    f.write("NTHEMES      equ %d\n" % NTHEMES)
    f.write("MUSIC_COUNT  equ %d\n" % len(music_song))
    f.write("TILE_BLOCK   equ %d\n" % TILE_BLOCK)
    f.write("ICON_BASE    equ %d\n" % ICON_BASE)
    f.write("NTILES       equ %d\n" % NTILES)

# ---- emit the binary asset data --------------------------------------------------
def row(vals):
    return "    db " + ",".join(str(v) for v in vals) + "\n"
with open(os.path.join(HERE, "build", "ws_data.inc"), "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by ws/mkdata.py - do not edit\n")
    f.write("tiles_data:\n")
    for t in range(NTILES):
        f.write(row(tiles[t]))
    f.write("tiles_end:\n")
    f.write("piece_masks:\n")
    for i in range(7):
        f.write("    dw " + ",".join("0x%04X" % piece_masks[i * 4 + r] for r in range(4)) + "\n")
    f.write("piece_pal:\n" + row(piece_pal))
    f.write("music_song:\n")
    for per, fr in music_song:
        f.write("    dw 0x%04X\n    db %d,0\n" % (per, fr))      # period word, frames byte, pad
    f.write("theme_pools:\n")
    for th in THEMES:
        f.write(row(th))

print("wrote ws/build/ws_data.inc + ws_equ.inc: %d tiles, %d icons, %d notes, %d themes" %
      (NTILES, NICONS, len(music_song), NTHEMES))
