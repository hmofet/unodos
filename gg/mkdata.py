#!/usr/bin/env python3
"""Generate gg/gen_data.inc from the shared UnoDOS assets (Sega Game Gear).

The Game Gear is SMS silicon — the same Z80 + 315-5124 VDP, so tiles are the
SAME 4 BITPLANE format as the SMS port (each row = 4 bytes plane0..plane3, 32
bytes/tile). The ONE hardware difference we exercise here is CRAM: the GG stores
**12-bit** colour (2 bytes per entry: low = %GGGGRRRR, high = %0000BBBB) instead
of the SMS's 6-bit single byte. And the GG LCD shows only the centre 160x144 of
the 256x192 frame, so this port uses the Game Boy's 20x18 `minimal` layout — a
vertical mini-icon list, drawn at a (6,3) cell offset into the visible window.

Tile map (loaded contiguously from VRAM tile 0):
    0           solid desktop blue
    1..95       font, fg=index1 (white) on bg=index2 (blue)  -> ASCII 32..126
    96..190     INVERTED font, fg=index2 on bg=index1        -> title bars
    191         solid white (index1)
    192         solid cyan  (index3)
    193         solid magenta (index4)
    194..       icons: 4 tiles each (16x16, from the x86 .BIN donors)
    ...         game block solids (red/green/yellow/orange) for Dostris
    ...         mini-icons: 1 tile each (16x16 donor downscaled to 8x8)

Usage: python gg/mkdata.py    (from the repo root)
"""
import re, sys, os

OUT = "gg/gen_data.inc"

# ---- shared 8x8 font ----------------------------------------------------------
font = []
for line in open("kernel/font8x8.asm", encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

def planar(pixrows):
    """8 rows of 8 palette indices (0..15) -> 32 bytes (4bpp planar)."""
    out = []
    for row in pixrows:
        for plane in range(4):
            b = 0
            for x in range(8):
                if (row[x] >> plane) & 1:
                    b |= 0x80 >> x
            out.append(b)
    return out

tiles = []   # (comment, [32 bytes])

tiles.append(("blank/desktop", planar([[2] * 8] * 8)))           # index2 = blue desktop

for g in range(95):                                              # font fg=1 on bg=2
    rows = [[1 if r & (0x80 >> b) else 2 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"'{chr(32+g)}'", planar(rows)))

for g in range(95):                                              # inverted font fg=2 on bg=1
    rows = [[2 if r & (0x80 >> b) else 1 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"inv '{chr(32+g)}'", planar(rows)))

tiles.append(("solid white(1)",   planar([[1] * 8] * 8)))        # T_WHITE
tiles.append(("solid cyan(3)",    planar([[3] * 8] * 8)))        # T_SOLC
tiles.append(("solid magenta(4)", planar([[4] * 8] * 8)))        # T_SOLM

# ---- icons from the x86 .BIN donors (same as SMS/NES/GB) ----------------------
# 2bpp chunky 0-3 -> our palette indices: 0->2 blue, 1->3 cyan, 2->4 magenta, 3->1 white
icmap = {0: 2, 1: 3, 2: 4, 3: 1}
def icon_pix(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    pix = []
    for r in range(16):
        rowb = chunky[r * 4:(r + 1) * 4]
        pix.append([icmap[(rowb[px // 4] >> ((3 - (px % 4)) * 2)) & 3] for px in range(16)])
    return pix
def icon_tiles(pix):
    out = []
    for ty in (0, 8):
        for tx in (0, 8):
            out.append(planar([pix[ty + y][tx:tx + 8] for y in range(8)]))
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

ICON_BASE = len(tiles)
icon_pixmaps = []
for name, binfile in ICONS:
    if not os.path.exists(binfile):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    pix = icon_pix(binfile)
    icon_pixmaps.append(pix)
    for t in icon_tiles(pix):
        tiles.append((f"icon {name}", t))
paint_pix = [[icmap[int(c)] for c in row] for row in PAINT_ICON]
icon_pixmaps.append(paint_pix)
for t in icon_tiles(paint_pix):
    tiles.append(("icon paint", t))

# ---- game block solids for Dostris -------------------------------------------
GAME_SOLID_BASE = len(tiles)
GAME_SOLID_IDX = [8, 9, 7, 10]   # green, yellow, red, orange
for idx in GAME_SOLID_IDX:
    tiles.append((f"solid idx{idx}", planar([[idx] * 8] * 8)))

# ---- mini-icons: each 16x16 donor downscaled to 8x8 (nearest) ----------------
MINI_BASE = len(tiles)
for i, pix in enumerate(icon_pixmaps):
    small = [[pix[2 * y][2 * x] for x in range(8)] for y in range(8)]
    tiles.append((f"mini {i}", planar(small)))

assert len(tiles) <= 448, f"{len(tiles)} tiles (VRAM holds 448 bg tiles)"

# ---- Dostris tetromino tables (same Contract shape as every port) ------------
PIECES = {
    "I": ["....", "####", "....", "...."], "O": [".##.", ".##.", "....", "...."],
    "T": [".#..", "###.", "....", "...."], "S": [".##.", "##..", "....", "...."],
    "Z": ["##..", ".##.", "....", "...."], "J": ["#...", "###.", "....", "...."],
    "L": ["..#.", "###.", "....", "...."]}
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

# ---- Music: a PSG tune (SN76489, same as the SMS port) -----------------------
PSG_CLK = 3579545
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
music_song = [(round(PSG_CLK / (32 * NOTE_HZ[n])), d) for n, d in TUNE]

# ---- GG 12-bit palettes (4 theme presets) ------------------------------------
# GG CRAM entry = 2 bytes: low = (G<<4)|R, high = B; each channel 0..15.
# We scale the SMS-style 0..3 channels to 0..15 (*5) so the look matches the SMS.
def gg(r, g, b):
    r, g, b = r * 5, g * 5, b * 5
    return [(g << 4) | r, b]
# index: 0 desktop, 1 white, 2 blue(text bg), 3 cyan, 4 magenta, 5 black, 6 dkgray,
#        7 red, 8 green, 9 yellow, 10 orange, 11..15 unused
def theme(desktop):
    pal = [desktop, gg(3, 3, 3), desktop, gg(0, 2, 2), gg(2, 0, 2), gg(0, 0, 0),
           gg(1, 1, 1), gg(3, 0, 0), gg(0, 3, 0), gg(3, 3, 0), gg(3, 1, 0)]
    pal += [gg(0, 0, 0)] * 5
    return [b for c in pal for b in c]          # flatten to 32 bytes
NTHEMES = 4
THEMES = [theme(gg(0, 0, 2)),   # blue
          theme(gg(0, 2, 0)),   # green
          theme(gg(2, 0, 0)),   # red
          theme(gg(1, 1, 1))]   # grey

# ---- emit ---------------------------------------------------------------------
def tilebytes(b):
    return "\n".join("    db " + ",".join(f"${v:02X}" for v in b[i:i+16])
                     for i in range(0, len(b), 16))

NORMAL_FONT_BASE = 1
INV_FONT_BASE = 96
T_SOLW = INV_FONT_BASE + 95     # 191
T_SOLC = T_SOLW + 1
T_SOLM = T_SOLW + 2
T_SGREEN, T_SYELLOW, T_SRED, T_SORANGE = (GAME_SOLID_BASE, GAME_SOLID_BASE + 1,
                                          GAME_SOLID_BASE + 2, GAME_SOLID_BASE + 3)
piece_tiles = [T_SOLC, T_SYELLOW, T_SOLM, T_SGREEN, T_SRED, T_SOLW, T_SORANGE]

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by gg/mkdata.py - do not edit\n\n")
    f.write(f"NTILES        EQU {len(tiles)}\n")
    f.write(f"T_FONT        EQU {NORMAL_FONT_BASE}\n")
    f.write(f"T_FONTINV     EQU {INV_FONT_BASE}\n")
    f.write(f"T_WHITE       EQU {T_SOLW}\n")
    f.write(f"T_SOLC        EQU {T_SOLC}\n")
    f.write(f"T_SOLM        EQU {T_SOLM}\n")
    f.write(f"T_ICONS       EQU {ICON_BASE}\n")
    f.write(f"NICONS        EQU {NICONS}\n")
    f.write(f"T_SGREEN      EQU {T_SGREEN}\n")
    f.write(f"T_SYELLOW     EQU {T_SYELLOW}\n")
    f.write(f"T_SRED        EQU {T_SRED}\n")
    f.write(f"T_SORANGE     EQU {T_SORANGE}\n")
    f.write(f"T_MINI        EQU {MINI_BASE}\n")
    f.write(f"NTHEMES       EQU {NTHEMES}\n")
    f.write(f"MUSIC_COUNT   EQU {len(music_song)}\n\n")
    f.write("tiles_all:\n")
    for comment, b in tiles:
        f.write(f"    ; tile - {comment}\n{tilebytes(b)}\n")
    f.write("\n; GG 12-bit BG palettes (16 colours x 2 bytes = 32 bytes per preset)\n")
    f.write("theme_pals:\n")
    for ti, pal in enumerate(THEMES):
        f.write(f"    ; theme {ti}\n    db " + ",".join(f"${v:02X}" for v in pal) + "\n")
    f.write("\n; Dostris: 7 pieces x 4 rotations, each a 4x4 bitmask (bit15=row0col0)\n")
    f.write("piece_masks:\n")
    for i, p in enumerate(piece_order):
        f.write("    dw " + ",".join(f"${piece_masks[i*4+r]:04X}" for r in range(4))
                + f"   ; {p}\n")
    f.write("piece_tiles:\n    db " + ",".join(str(t) for t in piece_tiles) + "\n")
    f.write("\n; Music: per note -> dw period, db frames\n")
    f.write("music_song:\n")
    for period, frames in music_song:
        f.write(f"    dw ${period:04X}\n    db {frames}\n")

print(f"wrote {OUT}: {len(tiles)} tiles, {NICONS} icons, mini@{MINI_BASE}, "
      f"{len(music_song)} notes")
