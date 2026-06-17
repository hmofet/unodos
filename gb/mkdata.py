#!/usr/bin/env python3
"""Generate gb/build/tiles.bin + gb/gb_data.inc from the shared UnoDOS assets.

Game Boy CHR tiles are 8x8 @ 2bpp = 16 bytes, but the format is INTERLEAVED by
row (NOT planar like the NES): each of the 8 rows is two bytes — byte0 = low
bitplane, byte1 = high bitplane — and a pixel's colour index 0..3 is
(hi_bit<<1)|lo_bit. The BG palette (BGP on DMG, or BG palette RAM on GBC) colours
the indices. There is no fixed backdrop quirk like the NES, so the "inverted"
title font is just a second font set (fg=index0 on bg=index1).

Tile map (loaded contiguously to VRAM $8000, BG uses the $8000 base):
    0           blank (index 0 -> desktop)
    1..95       font, fg=index1 on bg=index0           -> ASCII 32..126
    96..190     INVERTED font, fg=index0 on bg=index1  -> title bars
    191         solid index1 block (title-bar bg)
    192..235    icons: 4 tiles each (16x16, from the x86 .BIN donors)
    236..238    solid blocks (index1 / index2 / index3) -> Dostris pieces
    239..       mini-icons: 1 tile each (16x16 donor downscaled to 8x8) -> the
                vertical-list launcher

Palette: the GB is the Contract's MINIMAL profile on a 160x144 LCD. On DMG it is
4 greys via BGP; on GBC we write a real 4-colour BG palette (the UnoDOS theme:
blue / white / cyan / magenta) so the same ROM is colour on a Color and greyscale
on an original — detected at runtime.

Usage: python gb/mkdata.py   (from the repo root)
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
TILE_OUT = os.path.join(HERE, "build", "tiles.bin")
EQU_OUT = os.path.join(HERE, "gb_equ.inc")    # DEF constants (included early)
INC_OUT = os.path.join(HERE, "gb_data.inc")   # ROM data tables

# ---- shared 8x8 font (the same kernel/font8x8.asm every port uses) -------------
font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

def gb_tile(pixrows):
    """8 rows of 8 palette indices (0..3) -> 16 bytes (lo,hi interleaved per row)."""
    out = []
    for row in pixrows:
        lo = hi = 0
        for x in range(8):
            v = row[x] & 3
            if v & 1:
                lo |= 0x80 >> x
            if v & 2:
                hi |= 0x80 >> x
        out.append(lo)
        out.append(hi)
    return out

tiles = []   # (comment, [16 bytes])

# tile 0: blank (index 0 = desktop)
tiles.append(("blank/desktop", gb_tile([[0] * 8] * 8)))

# normal font: fg=1 on bg=0
for g in range(95):
    rows = [[1 if r & (0x80 >> b) else 0 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"'{chr(32+g)}'", gb_tile(rows)))

# inverted font: fg=0 on bg=1 -> title bars
for g in range(95):
    rows = [[0 if r & (0x80 >> b) else 1 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"inv '{chr(32+g)}'", gb_tile(rows)))

# solid index1 block -> title-bar background
tiles.append(("white block", gb_tile([[1] * 8] * 8)))

# ---- icons from the x86 .BIN headers (2bpp chunky, same donors as NES/SMS) ------
# donor chunky value -> GB palette index: 0->0 bg, 1->2 accent1, 2->3 accent2, 3->1 white
icmap = {0: 0, 1: 2, 2: 3, 3: 1}
def icon_pix(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    pix = []
    for r in range(16):
        row = chunky[r * 4:(r + 1) * 4]
        pix.append([icmap[(row[px // 4] >> ((3 - (px % 4)) * 2)) & 3] for px in range(16)])
    return pix
def icon_tiles(pix):
    out = []
    for ty in (0, 8):
        for tx in (0, 8):
            out.append(gb_tile([pix[ty + y][tx:tx + 8] for y in range(8)]))
    return out

ICONS = [("sysinfo", "build/sysinfo.bin"), ("clock", "build/clock.bin"),
         ("notepad", "build/notepad.bin"), ("music", "build/music.bin"),
         ("files", "build/browser.bin"), ("theme", "build/settings.bin"),
         ("tracker", "build/tracker.bin"), ("dostris", "build/tetris.bin"),
         ("outlast", "build/outlast.bin"), ("pacman", "build/pacman.bin")]
NICONS = len(ICONS) + 1   # + synthesized paint

PAINT_ICON = [
    "0000000000033000", "0000000000333000", "0000000003330000", "0000000033300000",
    "0000000333000000", "0000011330000000", "0000111100000000", "0000111000000000",
    "0001110000000000", "0001100000000000", "0022000022220000", "0222200222222000",
    "2222222222222200", "2222222222222220", "0222222222222200", "0002222222220000"]

icon_pixmaps = []   # full 16x16 pixmap per icon (for the mini-icons)
for name, binfile in ICONS:
    p = os.path.join(ROOT, binfile)
    if not os.path.exists(p):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    pix = icon_pix(p)
    icon_pixmaps.append(pix)
    for t in icon_tiles(pix):
        tiles.append((f"icon {name}", t))
paint_pix = [[icmap[int(c)] for c in row] for row in PAINT_ICON]
icon_pixmaps.append(paint_pix)
for t in icon_tiles(paint_pix):
    tiles.append(("icon paint", t))

# ---- solid block tiles for Dostris pieces (index 1/2/3) -------------------------
SOLID_BASE = len(tiles)
tiles.append(("solid 1", gb_tile([[1] * 8] * 8)))   # T_SOLW (also 191)
tiles.append(("solid 2", gb_tile([[2] * 8] * 8)))   # T_SOLC
tiles.append(("solid 3", gb_tile([[3] * 8] * 8)))   # T_SOLM

# ---- mini-icons: each 16x16 donor downscaled to 8x8 (nearest, every 2nd px) -----
MINI_BASE = len(tiles)
for i, pix in enumerate(icon_pixmaps):
    small = [[pix[2 * y][2 * x] for x in range(8)] for y in range(8)]
    tiles.append((f"mini {i}", gb_tile(small)))

assert len(tiles) <= 256, f"{len(tiles)} tiles exceeds 256"

# ---- Dostris tetromino tables (same Contract shape as the NES/SMS ports) --------
PIECES = {
    "I": ["....", "####", "....", "...."],
    "O": [".##.", ".##.", "....", "...."],
    "T": [".#..", "###.", "....", "...."],
    "S": [".##.", "##..", "....", "...."],
    "Z": ["##..", ".##.", "....", "...."],
    "J": ["#...", "###.", "....", "...."],
    "L": ["..#.", "###.", "....", "...."],
}
def rot90(grid):
    return ["".join(grid[3 - c][r] for c in range(4)) for r in range(4)]
def mask(grid):
    m = 0
    for r in range(4):
        for c in range(4):
            if grid[r][c] == "#":
                m |= 1 << (15 - (r * 4 + c))
    return m
piece_order = ["I", "O", "T", "S", "Z", "J", "L"]
piece_masks = []
for p in piece_order:
    g = PIECES[p]
    for _ in range(4):
        piece_masks.append(mask(g))
        g = rot90(g)

# ---- Music: a GB APU tune (channel 1, pulse) ------------------------------------
# GB freq reg x: actual = 131072 / (2048 - x)  ->  x = round(2048 - 131072/freq).
NOTE_HZ = {"C4": 262, "D4": 294, "E4": 330, "F4": 349, "G4": 392, "A4": 440,
           "B4": 494, "C5": 523, "D5": 587}
Q, H = 26, 52        # quarter / half note, in 60Hz frames
TUNE = [  # Ode to Joy — the same melody the NES/SMS ports play
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("E4", H), ("D4", Q), ("D4", H),
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("D4", H), ("C4", Q), ("C4", H),
]
music_song = [(round(2048 - 131072 / NOTE_HZ[n]) & 0x7FF, d) for n, d in TUNE]

# ---- palettes (4 theme presets, cycled by the Theme app) -----------------------
# DMG BGP byte: 2 bits per index (00 lightest .. 11 darkest). We vary it per theme
# so the Theme button is visible even on greyscale; on GBC we also write real BG
# palette RAM (BGR555). index0 = desktop, 1 = white/fg, 2/3 = accents.
def bgr555(r, g, b):
    return (b << 10) | (g << 5) | r
NTHEMES = 4
# (BGP byte, [4 BGR555 colours])  — index0 desktop, 1 white, 2 accent1, 3 accent2
THEMES = [
    (0x93, [bgr555(4, 8, 24),  bgr555(31, 31, 31), bgr555(0, 28, 28),  bgr555(28, 0, 28)]),   # blue
    (0xE4, [bgr555(2, 14, 4),  bgr555(31, 31, 31), bgr555(12, 28, 8),  bgr555(28, 30, 4)]),   # green
    (0x1B, [bgr555(22, 2, 2),  bgr555(31, 31, 31), bgr555(31, 14, 2),  bgr555(31, 28, 4)]),   # red
    (0x6C, [bgr555(2, 2, 4),   bgr555(31, 31, 31), bgr555(10, 10, 12), bgr555(20, 20, 22)]),  # mono
]

# ---- emit tiles.bin -------------------------------------------------------------
os.makedirs(os.path.dirname(TILE_OUT), exist_ok=True)
blob = bytearray()
for _, t in tiles:
    blob += bytes(t)
open(TILE_OUT, "wb").write(blob)

# ---- emit gb_equ.inc (DEF constants — included early; rgbds needs them first) ---
FONT_BASE, INV_BASE, WHITE_TILE, ICON_BASE = 1, 96, 191, 192
T_SOLW, T_SOLC, T_SOLM = SOLID_BASE, SOLID_BASE + 1, SOLID_BASE + 2
piece_tiles = [T_SOLC, T_SOLM, T_SOLW, T_SOLC, T_SOLM, T_SOLW, T_SOLC]  # I O T S Z J L
with open(EQU_OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by gb/mkdata.py - do not edit\n")
    f.write(f"DEF T_FONT     EQU {FONT_BASE}   ; 1..95 = ASCII 32..126\n")
    f.write(f"DEF T_FONTINV  EQU {INV_BASE}  ; inverted font - title bars\n")
    f.write(f"DEF T_WHITE    EQU {WHITE_TILE}  ; solid index1 block\n")
    f.write(f"DEF T_ICONS    EQU {ICON_BASE}  ; 4 tiles per icon\n")
    f.write(f"DEF T_SOLW     EQU {T_SOLW}  ; solid block index1\n")
    f.write(f"DEF T_SOLC     EQU {T_SOLC}  ; solid block index2\n")
    f.write(f"DEF T_SOLM     EQU {T_SOLM}  ; solid block index3\n")
    f.write(f"DEF T_MINI     EQU {MINI_BASE}  ; 1 mini-icon tile per app\n")
    f.write(f"DEF NICONS     EQU {NICONS}\n")
    f.write(f"DEF NTILES     EQU {len(tiles)}\n")
    f.write(f"DEF NTHEMES    EQU {NTHEMES}\n")
    f.write(f"DEF MUSIC_COUNT EQU {len(music_song)}\n")

# ---- emit gb_data.inc (ROM data tables) -----------------------------------------
with open(INC_OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by gb/mkdata.py - do not edit\n")
    f.write("; theme DMG BGP bytes (one per preset)\n")
    f.write("theme_bgp:\n    db " + ",".join(f"${b:02X}" for b, _ in THEMES) + "\n")
    f.write("; theme GBC BG palettes (4 BGR555 colours = 8 bytes per preset)\n")
    f.write("theme_pals:\n")
    for _, cols in THEMES:
        f.write("    dw " + ",".join(f"${c:04X}" for c in cols) + "\n")
    f.write("\n; Dostris: 7 pieces x 4 rotations, each a 4x4 bitmask (bit15=row0col0)\n")
    f.write("piece_masks:\n")
    for i, p in enumerate(piece_order):
        f.write("    dw " + ",".join(f"${piece_masks[i*4+r]:04X}" for r in range(4))
                + f"   ; {p}\n")
    f.write("piece_tiles:\n    db " + ",".join(str(t) for t in piece_tiles) + "\n")
    f.write("\n; Music: per note -> db freq-lo, freq-hi, frames\n")
    f.write("music_song:\n")
    for x, frames in music_song:
        f.write(f"    db ${x & 0xFF:02X},${(x >> 8) & 0xFF:02X},{frames}\n")

print(f"wrote {TILE_OUT} ({len(blob)} bytes) + {EQU_OUT} + {INC_OUT}: "
      f"{len(tiles)} tiles, {NICONS} icons, {len(music_song)} notes")
