#!/usr/bin/env python3
"""Generate sms/gen_data.inc from the shared UnoDOS assets.

The Sega Master System VDP (315-5124) stores 8x8 tiles as 4 BITPLANES: each row
is 4 bytes (plane0..plane3), bit7 = leftmost pixel; a pixel's colour index 0..15
is plane0bit | plane1bit<<1 | plane2bit<<2 | plane3bit<<3. 32 bytes per tile.

Tile map (loaded contiguously from VRAM tile 0):
    0           solid desktop blue (name-table clears to this)
    1..95       font, fg = index 1 (white) on bg = index 2 (blue)  -> ASCII 32..126
    96..190     INVERTED font, fg = index 2 (blue) on bg = index 1 (white) -> title bar
    191         solid white  (index 1)
    192         solid cyan   (index 3)
    193         solid magenta(index 4)
    194..       icons: 4 tiles each (16x16 2bpp, pulled from the x86 .BIN headers)

Palette (CRAM background entries 0..15), SMS colour byte = %00BBGGRR (2 bits each),
derived from the four UnoDOS theme colours (Contract UI_PAL*):
    0 blue(desktop) 1 white(fg) 2 blue 3 cyan 4 magenta 5 black 6 dkgray

Usage: python sms/mkdata.py    (from the repo root)
"""
import re, sys, os

OUT = "sms/gen_data.inc"

# ---------------- shared 8x8 font (the same kernel/font8x8.asm every port uses) ---
font = []
for line in open("kernel/font8x8.asm", encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

def planar(pixrows):
    """pixrows: 8 lists of 8 palette indices (0..15) -> 32 bytes (4bpp planar)."""
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

# tile 0: solid desktop blue (index 2) — name table clears to this
tiles.append(("blank/desktop", planar([[2] * 8] * 8)))

# normal font: fg=1 (white) on bg=2 (blue), opaque cell
for g in range(95):
    rows = []
    for r in font[g * 8:(g + 1) * 8]:
        rows.append([1 if r & (0x80 >> b) else 2 for b in range(8)])
    ch = chr(32 + g)
    tiles.append(("'%s'" % (ch if ch != "'" else "quote"), planar(rows)))

# inverted font: fg=2 (blue) on bg=1 (white) — title bar text
for g in range(95):
    rows = []
    for r in font[g * 8:(g + 1) * 8]:
        rows.append([2 if r & (0x80 >> b) else 1 for b in range(8)])
    tiles.append(("inv '%s'" % chr(32 + g), planar(rows)))

# solids
tiles.append(("solid white(1)",   planar([[1] * 8] * 8)))
tiles.append(("solid cyan(3)",    planar([[3] * 8] * 8)))
tiles.append(("solid magenta(4)", planar([[4] * 8] * 8)))

# window-frame tiles: white (index 1) lines on the blue (index 2) content bg.
def frame(l=False, r=False, b=False):
    rows = []
    for y in range(8):
        row = []
        for x in range(8):
            on = (l and x == 0) or (r and x == 7) or (b and y == 7)
            row.append(1 if on else 2)
        rows.append(row)
    return planar(rows)
tiles.append(("edge left",   frame(l=True)))           # T_EDGEL
tiles.append(("edge right",  frame(r=True)))           # T_EDGER
tiles.append(("edge bottom", frame(b=True)))           # T_EDGEB
tiles.append(("corner BL",   frame(l=True, b=True)))   # T_CORNBL
tiles.append(("corner BR",   frame(r=True, b=True)))   # T_CORNBR

# ---------------- icons from the x86 .BIN headers (same donors as genesis) -------
# 2bpp chunky values 0-3 = UI colours 0-3 -> our palette indices 2,3,4,1
icmap = {0: 2, 1: 3, 2: 4, 3: 1}
def icon_tiles(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    pix = []
    for r in range(16):
        row = chunky[r * 4:(r + 1) * 4]
        pix.append([icmap[(row[px // 4] >> ((3 - (px % 4)) * 2)) & 3]
                    for px in range(16)])
    out = []   # TL, TR, BL, BR
    for ty in (0, 8):
        for tx in (0, 8):
            out.append(planar([pix[ty + y][tx:tx + 8] for y in range(8)]))
    return out

ICONS = [("sysinfo", "build/sysinfo.bin"), ("clock", "build/clock.bin"),
         ("notepad", "build/notepad.bin"), ("music", "build/music.bin"),
         ("files", "build/browser.bin"), ("theme", "build/settings.bin"),
         ("tracker", "build/tracker.bin"), ("dostris", "build/tetris.bin"),
         ("outlast", "build/outlast.bin"), ("pacman", "build/pacman.bin")]
NICONS = len(ICONS) + 1  # + synthesized paint
for name, binfile in ICONS:
    if not os.path.exists(binfile):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    for i, t in enumerate(icon_tiles(binfile)):
        tiles.append((f"icon {name} {i}", t))

# paint icon: synthesized (no x86 donor) — same 2bpp chunky semantics
PAINT_ICON = [
    "0000000000033000", "0000000000333000", "0000000003330000", "0000000033300000",
    "0000000333000000", "0000011330000000", "0000111100000000", "0000111000000000",
    "0001110000000000", "0001100000000000", "0022000022220000", "0222200222222000",
    "2222222222222200", "2222222222222220", "0222222222222200", "0002222222220000"]
pix = [[icmap[int(c)] for c in row] for row in PAINT_ICON]
for ty in (0, 8):
    for tx in (0, 8):
        tiles.append((f"icon paint {ty//8*2+tx//8}",
                      planar([pix[ty + y][tx:tx + 8] for y in range(8)])))

# ---------------- cursor sprite (UnoDOS arrow, 8x16 = 2 tiles) -------------------
# Drawn in the SPRITE palette (same colours): 1=white interior, 5=black outline,
# 0=transparent. The VDP needs an 8x16 sprite's tile number to be EVEN, so pad to
# an even base if the running tile count is odd.
ARROW = [
    "O.......", "OO......", "OWO.....", "OWWO....",
    "OWWWO...", "OWWWWO..", "OWWWWWO.", "OWWWWWWO",
    "OWWWWOOO", "OWWOWO..", "OWO.OWO.", "OO..OWO.",
    "O....OWO", ".....OWO", "......O.", "........"]
amap = {".": 0, "W": 1, "O": 5}
arows = [[amap[c] for c in row] for row in ARROW]
if len(tiles) % 2:
    tiles.append(("pad (cursor even-align)", planar([[0] * 8] * 8)))
CURSOR_BASE = len(tiles)
tiles.append(("cursor top",    planar(arows[:8])))
tiles.append(("cursor bottom", planar(arows[8:])))

# ---------------- game block solids (for Dostris) -------------------------------
# Extra solid tiles for palette indices the UI solids don't already cover.
GAME_SOLID_BASE = len(tiles)
GAME_SOLID_IDX = [8, 9, 7, 10]   # green, yellow, red, orange
for idx in GAME_SOLID_IDX:
    tiles.append((f"solid idx{idx}", planar([[idx] * 8] * 8)))

# ---------------- Pac-Man tiles -------------------------------------------------
# The maze walls reuse T_SOLC (cyan); these are the dot, power pellet, Pac-Man
# and ghost, drawn 8x8 on the blue window bg (index 2). Same 13x13 pillar-maze
# design as the NES port, but SMS tiles carry their own colour (no per-app
# palette), so each actor is its own coloured shape. 1=white 2=blue 9=yellow
# 4=magenta 5=black.
def pmtile(rows, m):
    return planar([[m[c] for c in r] for r in rows])

PM_BASE = len(tiles)
tiles.append(("pm dot", pmtile([
    "........", "........", "........", "...##...",
    "...##...", "........", "........", "........"], {".": 2, "#": 1})))
tiles.append(("pm power", pmtile([
    "........", "..####..", "..####..", "..####..",
    "..####..", "..####..", "........", "........"], {".": 2, "#": 9})))
tiles.append(("pm pac", pmtile([          # yellow disc, wedge mouth facing right
    "..####..", ".######.", "####....", "###.....",
    "####....", ".######.", "..####..", "........"], {".": 2, "#": 9})))
tiles.append(("pm ghost", pmtile([        # magenta body, white eyes, wavy skirt
    "..####..", ".######.", "#oo##oo#", "#oo##oo#",
    "########", "########", "########", "#.##.##."], {".": 2, "#": 4, "o": 1})))

# ---------------- palette (CRAM bg entries 0..15) --------------------------------
# SMS colour = %00BBGGRR; 2 bits per channel (0..3).
def rgb(r, g, b):
    return (b << 4) | (g << 2) | r
PALETTE = [
    rgb(0, 0, 2),   # 0 blue (desktop)
    rgb(3, 3, 3),   # 1 white (fg)
    rgb(0, 0, 2),   # 2 blue (text bg = desktop)
    rgb(0, 2, 2),   # 3 cyan (accent1)
    rgb(2, 0, 2),   # 4 magenta (accent2)
    rgb(0, 0, 0),   # 5 black
    rgb(1, 1, 1),   # 6 dark gray
    rgb(3, 0, 0),   # 7 red    (game)
    rgb(0, 3, 0),   # 8 green  (game)
    rgb(3, 3, 0),   # 9 yellow (game)
    rgb(3, 1, 0),   # 10 orange (game)
] + [0] * 5

# ---------------- Dostris tetromino tables --------------------------------------
# Each piece = 4 rotations, each a 4x4 grid (bit15 = row0/col0, scanning L->R,
# T->B). Emitted as 28 words. Each piece also carries a board-cell tile.
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
    # rotate a 4x4 grid clockwise
    return ["".join(grid[3 - c][r] for c in range(4)) for r in range(4)]
def mask(grid):
    m = 0
    for r in range(4):
        for c in range(4):
            if grid[r][c] == "#":
                m |= 1 << (15 - (r * 4 + c))
    return m
piece_order = ["I", "O", "T", "S", "Z", "J", "L"]
piece_masks = []      # 7*4 words
for p in piece_order:
    g = PIECES[p]
    for _ in range(4):
        piece_masks.append(mask(g))
        g = rot90(g)

# ---------------- Music: a PSG tune (SN76489, same chip family as Genesis) -------
# Each note -> (10-bit tone period, duration in 60Hz frames). The SMS PSG clock
# is 3.579545 MHz (NTSC); period = clk / (32 * freq).
PSG_CLK = 3579545
NOTE_HZ = {"C4": 262, "D4": 294, "E4": 330, "F4": 349, "G4": 392, "A4": 440,
           "B4": 494, "C5": 523, "D5": 587}
Q, H = 26, 52        # quarter / half note, in frames (~tempo)
TUNE = [  # Ode to Joy (Beethoven)
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("E4", H), ("D4", Q), ("D4", H),
    ("E4", Q), ("E4", Q), ("F4", Q), ("G4", Q), ("G4", Q), ("F4", Q),
    ("E4", Q), ("D4", Q), ("C4", Q), ("C4", Q), ("D4", Q), ("E4", Q),
    ("D4", H), ("C4", Q), ("C4", H),
]
music_song = [(round(PSG_CLK / (32 * NOTE_HZ[n])), d) for n, d in TUNE]

# ---------------- emit ------------------------------------------------------------
def tilebytes(b):
    return "\n".join("    db " + ",".join(f"${v:02X}" for v in b[i:i+16])
                     for i in range(0, len(b), 16))

# tile indices (computed from the build order, not hand-numbered)
NORMAL_FONT_BASE = 1
INV_FONT_BASE    = 96
T_SOLW           = INV_FONT_BASE + 95            # 191
T_SOLC           = T_SOLW + 1
T_SOLM           = T_SOLW + 2
FRAME_BASE       = T_SOLW + 3                     # edgeL/R/B, cornBL/BR
ICON_BASE        = FRAME_BASE + 5                 # 199

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by sms/mkdata.py - do not edit\n\n")
    f.write(f"NTILES        EQU {len(tiles)}\n")
    f.write(f"T_FONT        EQU {NORMAL_FONT_BASE}   ; 1..95 = ASCII 32..126 (white on blue)\n")
    f.write(f"T_FONTINV     EQU {INV_FONT_BASE}  ; inverted font (blue on white) - title bars\n")
    f.write(f"T_SOLW        EQU {T_SOLW}  ; solid white\n")
    f.write(f"T_SOLC        EQU {T_SOLC}  ; solid cyan\n")
    f.write(f"T_SOLM        EQU {T_SOLM}  ; solid magenta\n")
    f.write(f"T_EDGEL       EQU {FRAME_BASE}\n")
    f.write(f"T_EDGER       EQU {FRAME_BASE+1}\n")
    f.write(f"T_EDGEB       EQU {FRAME_BASE+2}\n")
    f.write(f"T_CORNBL      EQU {FRAME_BASE+3}\n")
    f.write(f"T_CORNBR      EQU {FRAME_BASE+4}\n")
    f.write(f"T_ICONS       EQU {ICON_BASE}  ; 4 tiles per icon\n")
    f.write(f"NICONS        EQU {NICONS}\n")
    f.write(f"T_CURSOR      EQU {CURSOR_BASE}  ; 8x16 sprite (sprite pattern base $0000)\n")
    # game block solids: GAME_SOLID_BASE+0..3 = idx 8(green),9(yellow),7(red),10(orange)
    T_SGREEN  = GAME_SOLID_BASE
    T_SYELLOW = GAME_SOLID_BASE + 1
    T_SRED    = GAME_SOLID_BASE + 2
    T_SORANGE = GAME_SOLID_BASE + 3
    f.write(f"T_SGREEN      EQU {T_SGREEN}\n")
    f.write(f"T_SYELLOW     EQU {T_SYELLOW}\n")
    f.write(f"T_SRED        EQU {T_SRED}\n")
    f.write(f"T_SORANGE     EQU {T_SORANGE}\n")
    # Pac-Man: dot, power pellet, Pac-Man, ghost (walls reuse T_SOLC)
    f.write(f"T_PMDOT       EQU {PM_BASE}\n")
    f.write(f"T_PMPOW       EQU {PM_BASE+1}\n")
    f.write(f"T_PMPAC       EQU {PM_BASE+2}\n")
    f.write(f"T_PMGHO       EQU {PM_BASE+3}\n\n")
    f.write("; 4bpp planar tile blob, uploaded contiguously to VRAM tile 0\n")
    f.write("tiles_all:\n")
    for comment, b in tiles:
        f.write(f"    ; tile - {comment}\n{tilebytes(b)}\n")
    f.write("\n; CRAM background palette (16 entries; also copied to the sprite palette)\n")
    f.write("palette:\n    db " + ",".join(f"${v:02X}" for v in PALETTE) + "\n")
    # Dostris tables: I O T S Z J L
    piece_tiles = [T_SOLC, T_SYELLOW, T_SOLM, T_SGREEN, T_SRED, T_SOLW, T_SORANGE]
    f.write("\n; Dostris: 7 pieces x 4 rotations, each a 4x4 bitmask (bit15=row0col0)\n")
    f.write("piece_masks:\n")
    for i, p in enumerate(piece_order):
        f.write("    dw " + ",".join(f"${piece_masks[i*4+r]:04X}" for r in range(4))
                + f"   ; {p}\n")
    f.write("piece_tiles:\n    db " + ",".join(str(t) for t in piece_tiles) + "\n")
    # Music: per note -> dw period, db frames (3 bytes each)
    f.write(f"\nMUSIC_COUNT   EQU {len(music_song)}\n")
    f.write("music_song:\n")
    for period, frames in music_song:
        f.write(f"    dw ${period:04X}\n    db {frames}\n")

print(f"wrote {OUT}: {len(tiles)} tiles ({len(tiles)*32} bytes), {NICONS} icons, "
      f"cursor@{CURSOR_BASE}, game solids@{GAME_SOLID_BASE}, {len(music_song)} notes")
