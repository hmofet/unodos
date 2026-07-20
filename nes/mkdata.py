#!/usr/bin/env python3
"""Generate nes/build/chr.bin + nes/nes_data.inc from the shared UnoDOS assets.

NES CHR tiles are 8x8 @ 2bpp = 16 bytes: 8 bytes of bitplane 0 (low bit of each
pixel) then 8 bytes of bitplane 1 (high bit). A pixel's colour index 0..3 picks
a colour from the background palette — EXCEPT index 0, which the PPU always
renders as the universal backdrop ($3F00). So "inverted" title-bar text can't be
a palette swap (its white background would be index 0 = backdrop); we generate a
real inverted font whose foreground is index 0 (the blue backdrop) on an index-1
(white) background.

Pattern table 0 (background tiles, 256 max):
    0           blank (all index 0 -> blue desktop / backdrop)
    1..95       font, fg=index1 (white) on bg=index0 (blue)   -> ASCII 32..126
    96..190     INVERTED font, fg=index0 (blue) on bg=index1 (white) -> title bar
    191         white block (all index1) -> title-bar background
    192..       icons: 4 tiles each (16x16 @ 2bpp from the x86 .BIN headers)

Palette 0 (the only one used; attribute table is all zeros):
    $3F00 backdrop = blue ; +1 white ; +2 cyan ; +3 magenta.

Usage: python nes/mkdata.py   (from the repo root)
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
CHR_OUT = os.path.join(HERE, "build", "chr.bin")
INC_OUT = os.path.join(HERE, "nes_data.inc")

# ---- shared 8x8 font (the same kernel/font8x8.asm every port uses) -------------
font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

def nes_tile(pixrows):
    """8 rows of 8 palette indices (0..3) -> 16 bytes (plane0 x8, plane1 x8)."""
    p0, p1 = [], []
    for row in pixrows:
        b0 = b1 = 0
        for x in range(8):
            v = row[x] & 3
            if v & 1:
                b0 |= 0x80 >> x
            if v & 2:
                b1 |= 0x80 >> x
        p0.append(b0)
        p1.append(b1)
    return p0 + p1

tiles = []   # (comment, [16 bytes])

# tile 0: blank (index 0 = blue backdrop)
tiles.append(("blank/desktop", nes_tile([[0] * 8] * 8)))

# normal font: fg=1 (white) on bg=0 (blue)
for g in range(95):
    rows = [[1 if r & (0x80 >> b) else 0 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"'{chr(32+g)}'", nes_tile(rows)))

# inverted font: fg=0 (blue) on bg=1 (white) — title bar
for g in range(95):
    rows = [[0 if r & (0x80 >> b) else 1 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"inv '{chr(32+g)}'", nes_tile(rows)))

# white block (all index 1) — title-bar background
tiles.append(("white block", nes_tile([[1] * 8] * 8)))

# ---- icons from the x86 .BIN headers (2bpp chunky, same donors as SMS) ----------
# donor chunky value -> NES palette index: 0->0 blue, 1->2 cyan, 2->3 magenta, 3->1 white
icmap = {0: 0, 1: 2, 2: 3, 3: 1}
def icon_tiles(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    pix = []
    for r in range(16):
        row = chunky[r * 4:(r + 1) * 4]
        pix.append([icmap[(row[px // 4] >> ((3 - (px % 4)) * 2)) & 3] for px in range(16)])
    out = []
    for ty in (0, 8):
        for tx in (0, 8):
            out.append(nes_tile([pix[ty + y][tx:tx + 8] for y in range(8)]))
    return out

ICONS = [("sysinfo", "build/sysinfo.bin"), ("clock", "build/clock.bin"),
         ("notepad", "build/notepad.bin"), ("music", "build/music.bin"),
         ("files", "build/browser.bin"), ("theme", "build/settings.bin"),
         ("tracker", "build/tracker.bin"), ("dostris", "build/tetris.bin"),
         ("outlast", "build/outlast.bin"), ("pacman", "build/pacman.bin")]
NICONS = len(ICONS) + 1
for name, binfile in ICONS:
    p = os.path.join(ROOT, binfile)
    if not os.path.exists(p):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    for t in icon_tiles(p):
        tiles.append((f"icon {name}", t))

PAINT_ICON = [
    "0000000000033000", "0000000000333000", "0000000003330000", "0000000033300000",
    "0000000333000000", "0000011330000000", "0000111100000000", "0000111000000000",
    "0001110000000000", "0001100000000000", "0022000022220000", "0222200222222000",
    "2222222222222200", "2222222222222220", "0222222222222200", "0002222222220000"]
pix = [[icmap[int(c)] for c in row] for row in PAINT_ICON]
for ty in (0, 8):
    for tx in (0, 8):
        tiles.append(("icon paint", nes_tile([pix[ty + y][tx:tx + 8] for y in range(8)])))

# ---- solid block tiles for Dostris pieces (one palette: blue/white/cyan/magenta) -
# Index 0 (blue) is the backdrop, so a "block" must be index 1/2/3. We get three
# distinct piece colours; the 7 tetrominoes cycle through them.
SOLID_BASE = len(tiles)
tiles.append(("solid white(1)",   nes_tile([[1] * 8] * 8)))   # T_SOLW (also 191)
tiles.append(("solid cyan(2)",    nes_tile([[2] * 8] * 8)))   # T_SOLC
tiles.append(("solid magenta(3)", nes_tile([[3] * 8] * 8)))   # T_SOLM

# ---- game glyph tiles (Pac-Man cells + the Paint cursor overlays) ---------------
# All background tiles (no sprite layer): the actors are recoloured CELLS. Under
# the Pac-Man palette index1=white (dots/ghosts/font), index2=blue (walls),
# index3=yellow (Pac-Man).
def glyph(rows, fg):
    return nes_tile([[fg if ch == "#" else 0 for ch in row] for row in rows])

EXTRA_BASE = len(tiles)
tiles.append(("pac dot", glyph([                # T_DOT (index1)
    "........", "........", "........", "...##...",
    "...##...", "........", "........", "........"], 1)))
tiles.append(("pac power", glyph([              # T_PWR (index1)
    "........", "..####..", ".######.", ".######.",
    ".######.", ".######.", "..####..", "........"], 1)))
tiles.append(("pac-man", glyph([                # T_PAC (index3 = yellow)
    "..####..", ".######.", "######..", "####....",
    "######..", ".######.", "..####..", "........"], 3)))
tiles.append(("ghost", glyph([                  # T_GHO (index1 = white)
    "..####..", ".######.", ".#.##.#.", ".######.",
    ".######.", ".######.", "#.#.#.#.", "........"], 1)))

# Paint cursor: a 1px border ring over an interior of colour i (i = the cell's
# ink). Border contrasts: index3 over a white cell, index1 otherwise.
CUR_BASE = len(tiles)
for i in range(4):
    b = 3 if i == 1 else 1
    ring = [[b] * 8] + [[b] + [i] * 6 + [b] for _ in range(6)] + [[b] * 8]
    tiles.append((f"paint cursor over {i}", nes_tile(ring)))

assert len(tiles) <= 256, f"{len(tiles)} tiles exceeds pattern table 0"

# ---- Dostris tetromino tables --------------------------------------------------
# Each piece = 4 rotations, each a 4x4 grid; emitted as 4 words (bit15 = row0col0,
# scanning L->R, T->B) — the exact shape the SMS port's get_mask/piece_collide
# consume, ported to 6502 here. Each piece also carries a board-cell tile.
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

# ---- Music: a 2A03 APU tune (pulse channel 1) ----------------------------------
# Pulse period = round(CPU / (16 * freq)) - 1, an 11-bit timer ($4002 + $4003 low3).
# NES NTSC CPU = 1.789773 MHz. The actual timbre is an ear-check like every port.
CPU_HZ = 1789773.0
NOTE_HZ = {"C4": 262, "D4": 294, "E4": 330, "F4": 349, "G4": 392, "A4": 440,
           "B4": 494, "C5": 523, "D5": 587}
Q, H = 26, 52        # quarter / half note, in 60Hz frames
TUNE = [  # Ode to Joy (Beethoven) — the same melody the SMS port plays
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("E4", H), ("D4", Q), ("D4", H),
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("D4", H), ("C4", Q), ("C4", H),
]
music_song = [(round(CPU_HZ / (16 * NOTE_HZ[n])) - 1, d) for n, d in TUNE]

# ---- Tracker: note period table (C4..C5, the same 8 notes the C64 grid uses) ----
TK_NOTES = ["C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5"]
tk_periods = [round(CPU_HZ / (16 * NOTE_HZ[n])) - 1 for n in TK_NOTES]

# ---- palette (NES master-palette indices) --------------------------------------
PAL = [0x11, 0x30, 0x2C, 0x24]   # blue(backdrop), white, cyan, magenta
# per-app $3F00 palettes (loaded on app entry; the launcher restores PAL on exit)
PAL_OUTLAST = [0x1A, 0x30, 0x00, 0x16]   # green grass(backdrop), white, grey road, red car
PAL_PACMAN  = [0x0F, 0x30, 0x12, 0x28]   # black(backdrop), white, blue walls, yellow pac

# ---- emit CHR (8KB: table 0 = our tiles padded to 4KB, table 1 = zeros) --------
os.makedirs(os.path.dirname(CHR_OUT), exist_ok=True)
chr_bytes = bytearray()
for _, t in tiles:
    chr_bytes += bytes(t)
chr_bytes += bytes(0x1000 - len(chr_bytes))   # pad table 0 to 4KB
chr_bytes += bytes(0x1000)                     # table 1 (sprites) empty for M1
assert len(chr_bytes) == 0x2000
open(CHR_OUT, "wb").write(chr_bytes)

FONT_BASE = 1
INV_BASE = 96
WHITE_TILE = 191
ICON_BASE = 192
# piece -> board tile (cycle the three available block colours)
T_SOLW, T_SOLC, T_SOLM = SOLID_BASE, SOLID_BASE + 1, SOLID_BASE + 2
piece_tiles = [T_SOLC, T_SOLM, T_SOLW, T_SOLC, T_SOLM, T_SOLW, T_SOLC]  # I O T S Z J L
with open(INC_OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by nes/mkdata.py - do not edit\n")
    f.write(f"T_FONT      EQU {FONT_BASE}   ; 1..95 = ASCII 32..126 (white on blue)\n")
    f.write(f"T_FONTINV   EQU {INV_BASE}  ; inverted font (blue on white) - title bar\n")
    f.write(f"T_WHITE     EQU {WHITE_TILE}  ; white block (title-bar bg)\n")
    f.write(f"T_ICONS     EQU {ICON_BASE}  ; 4 tiles per icon\n")
    f.write(f"NICONS      EQU {NICONS}\n")
    f.write(f"T_SOLW      EQU {T_SOLW}  ; solid white block\n")
    f.write(f"T_SOLC      EQU {T_SOLC}  ; solid cyan block\n")
    f.write(f"T_SOLM      EQU {T_SOLM}  ; solid magenta block\n")
    f.write(f"T_DOT       EQU {EXTRA_BASE}  ; Pac-Man dot (index1)\n")
    f.write(f"T_PWR       EQU {EXTRA_BASE+1}  ; Pac-Man power pellet (index1)\n")
    f.write(f"T_PAC       EQU {EXTRA_BASE+2}  ; Pac-Man actor (index3)\n")
    f.write(f"T_GHO       EQU {EXTRA_BASE+3}  ; ghost actor (index1)\n")
    f.write(f"T_CUR       EQU {CUR_BASE}  ; Paint cursor over ink 0..3 (4 tiles)\n")
    f.write("palette:\n    dc.b " + ",".join(f"${v:02X}" for v in PAL) + "\n")
    f.write("pal_outlast:\n    dc.b " + ",".join(f"${v:02X}" for v in PAL_OUTLAST) + "\n")
    f.write("pal_pacman:\n    dc.b " + ",".join(f"${v:02X}" for v in PAL_PACMAN) + "\n")
    f.write("; Tracker: pulse periods for notes C4 D4 E4 F4 G4 A4 B4 C5\n")
    f.write("tk_periods:\n    dc.w " + ",".join(f"${p:04X}" for p in tk_periods) + "\n")
    # Dostris piece tables (I O T S Z J L)
    f.write("\n; Dostris: 7 pieces x 4 rotations, each a 4x4 bitmask (bit15=row0col0)\n")
    f.write("piece_masks:\n")
    for i, p in enumerate(piece_order):
        f.write("    dc.w " + ",".join(f"${piece_masks[i*4+r]:04X}" for r in range(4))
                + f"   ; {p}\n")
    f.write("piece_tiles:\n    dc.b " + ",".join(str(t) for t in piece_tiles) + "\n")
    # Music (APU pulse): per note -> dw period (11-bit), db frames
    f.write(f"\nMUSIC_COUNT EQU {len(music_song)}\n")
    f.write("music_song:\n")
    for period, frames in music_song:
        f.write(f"    dc.w ${period:04X}\n    dc.b {frames}\n")

print(f"wrote {CHR_OUT} (8KB) + {INC_OUT}: {len(tiles)} tiles, {NICONS} icons, "
      f"{len(music_song)} notes")
