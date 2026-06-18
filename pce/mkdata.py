#!/usr/bin/env python3
"""Generate pce/pce_data.inc from the shared UnoDOS assets (NEC PC Engine).

The HuC6270 VDC uses 8x8 **4bpp** tiles = 32 bytes, stored as: 16 bytes of
planes 0&1 interleaved per row (row r: byte[2r]=plane0, byte[2r+1]=plane1) then
16 bytes of planes 2&3 (byte[16+2r]=plane2, byte[16+2r+1]=plane3). A pixel's
colour index 0..15 = p0 | p1<<1 | p2<<2 | p3<<3, picked from a 16-colour VCE
palette. The PC Engine screen is 256x224 = 32x28 BAT cells, so this port reuses
the NES's 4-column grid launcher; only the draw layer (VDC) differs.

Tiles are uploaded to VRAM word-address $1000 (CG index base $100), so a BAT
entry for tile N is (palette<<12) | ($100 + N).

Usage: python pce/mkdata.py    (from the repo root)
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(HERE, "pce_data.inc")

font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8

def pce_tile(rows):
    """8 rows of 8 indices (0..15) -> 32 bytes (planes 0&1 then 2&3)."""
    out = [0] * 32
    for r in range(8):
        p0 = p1 = p2 = p3 = 0
        for x in range(8):
            v = rows[r][x] & 15
            bit = 0x80 >> x
            if v & 1: p0 |= bit
            if v & 2: p1 |= bit
            if v & 4: p2 |= bit
            if v & 8: p3 |= bit
        out[r*2] = p0; out[r*2+1] = p1; out[16+r*2] = p2; out[16+r*2+1] = p3
    return out

tiles = []
tiles.append(("blank", pce_tile([[0]*8]*8)))                      # 0 = desktop (index0)
for g in range(95):                                              # font fg=1 on bg=0
    rows = [[1 if r & (0x80 >> b) else 0 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"'{chr(32+g)}'", pce_tile(rows)))
for g in range(95):                                             # inverted fg=0 on bg=1
    rows = [[0 if r & (0x80 >> b) else 1 for b in range(8)] for r in font[g*8:(g+1)*8]]
    tiles.append((f"inv'{chr(32+g)}'", pce_tile(rows)))
tiles.append(("white", pce_tile([[1]*8]*8)))                     # 191 solid index1

# icons: x86 donors, chunky 0-3 -> index 0(bg) 2(cyan) 3(magenta) 1(white)
icmap = {0: 0, 1: 2, 2: 3, 3: 1}
def icon_pix(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    return [[icmap[(chunky[r*4 + px//4] >> ((3-(px%4))*2)) & 3] for px in range(16)]
            for r in range(16)]
def icon_tiles(pix):
    return [pce_tile([pix[ty+y][tx:tx+8] for y in range(8)]) for ty in (0,8) for tx in (0,8)]
ICONS = [("sysinfo","build/sysinfo.bin"),("clock","build/clock.bin"),("notepad","build/notepad.bin"),
         ("music","build/music.bin"),("files","build/browser.bin"),("theme","build/settings.bin"),
         ("tracker","build/tracker.bin"),("dostris","build/tetris.bin"),("outlast","build/outlast.bin"),
         ("pacman","build/pacman.bin")]
NICONS = len(ICONS) + 1
PAINT = ["0000000000033000","0000000000333000","0000000003330000","0000000033300000",
         "0000000333000000","0000011330000000","0000111100000000","0000111000000000",
         "0001110000000000","0001100000000000","0022000022220000","0222200222222000",
         "2222222222222200","2222222222222220","0222222222222200","0002222222220000"]
ICON_BASE = len(tiles)
for name, bf in ICONS:
    p = os.path.join(ROOT, bf)
    if not os.path.exists(p):
        sys.exit(f"missing {bf} - run 'make floppy144' first")
    for t in icon_tiles(icon_pix(p)):
        tiles.append((f"icon {name}", t))
for t in icon_tiles([[icmap[int(c)] for c in row] for row in PAINT]):
    tiles.append(("paint", t))

# dostris block solids (indices 2,3,1 + game colours 5,6,7,8)
SOLID_BASE = len(tiles)
for idx in (1, 2, 3, 5, 6, 7, 8):
    tiles.append((f"solid{idx}", pce_tile([[idx]*8]*8)))
T_SOLW, T_SOLC, T_SOLM, T_SGREEN, T_SYELLOW, T_SRED, T_SORANGE = range(SOLID_BASE, SOLID_BASE+7)

# Dostris piece tables
PIECES = {"I":["....","####","....","...."],"O":[".##.",".##.","....","...."],
          "T":[".#..","###.","....","...."],"S":[".##.","##..","....","...."],
          "Z":["##..",".##.","....","...."],"J":["#...","###.","....","...."],
          "L":["..#.","###.","....","...."]}
def rot90(g): return ["".join(g[3-c][r] for c in range(4)) for r in range(4)]
def mask(g):
    m=0
    for r in range(4):
        for c in range(4):
            if g[r][c]=="#": m|=1<<(15-(r*4+c))
    return m
order=["I","O","T","S","Z","J","L"]
piece_masks=[]
for p in order:
    g=PIECES[p]
    for _ in range(4):
        piece_masks.append(mask(g)); g=rot90(g)
piece_tiles=[T_SOLC,T_SYELLOW,T_SOLM,T_SGREEN,T_SRED,T_SOLW,T_SORANGE]

# Music (PSG): per note -> period (12-bit) + frames. PCE PSG freq = 3.58MHz/(32*f).
PSG_CLK=3579545
NOTE_HZ={"C4":262,"D4":294,"E4":330,"F4":349,"G4":392,"A4":440,"B4":494,"C5":523,"D5":587}
Q,H=26,52
TUNE=[("E4",Q),("E4",Q),("F4",Q),("G4",Q),("G4",Q),("F4",Q),("E4",Q),("D4",Q),("C4",Q),("C4",Q),
      ("D4",Q),("E4",Q),("E4",H),("D4",Q),("D4",H),("E4",Q),("E4",Q),("F4",Q),("G4",Q),("G4",Q),
      ("F4",Q),("E4",Q),("D4",Q),("C4",Q),("C4",Q),("D4",Q),("E4",Q),("D4",H),("C4",Q),("C4",H)]
music_song=[(round(PSG_CLK/(32*NOTE_HZ[n]))&0xFFF,d) for n,d in TUNE]

# VCE palette: 9-bit colour, GGG RRR BBB layout (G bits 8-6, R bits 5-3, B bits 2-0)
# — the real HuC6260 order, verified empirically on Mednafen (gold-standard PCE
# emulator): writing colour0=$0007 rendered BLUE and colour1=$01C0 rendered GREEN,
# so the low field is blue, the high field green. 16-colour palette 0.
def vce(r,g,b): return (g<<6)|(r<<3)|b      # r,g,b in 0..7
#       0 blue        1 white     2 cyan     3 magenta  4 blk 5 green   6 yellow  7 red     8 orange
PAL=[vce(1,2,6), vce(7,7,7), vce(0,7,7), vce(7,0,7), 0, vce(0,7,0), vce(7,7,0), vce(7,0,0), vce(7,4,0)]
PAL+=[0]*(16-len(PAL))
NTHEMES=4
DESK=[vce(1,2,6), vce(1,5,1), vce(6,1,1), vce(3,3,4)]   # blue green red grey
THEMES=[]
for dk in DESK:
    pp=list(PAL); pp[0]=dk; THEMES.append(pp)

with open(OUT,"w",newline="\n") as f:
    f.write("; AUTO-GENERATED by pce/mkdata.py - do not edit\n")
    f.write(f"NTILES   = {len(tiles)}\n")
    f.write(f"T_FONT   = 1\nT_FONTINV = 96\nT_WHITE  = 191\n")
    f.write(f"T_ICONS  = {ICON_BASE}\nNICONS = {NICONS}\n")
    f.write(f"T_SOLW = {T_SOLW}\nT_SOLC = {T_SOLC}\nT_SOLM = {T_SOLM}\n")
    f.write(f"NTHEMES = {NTHEMES}\nMUSIC_COUNT = {len(music_song)}\n")
    f.write("\n.export tiles_all, theme_pals, piece_masks, piece_tiles, music_song\n")
    f.write(".segment \"RODATA\"\n")
    f.write("tiles_all:\n")
    for c,b in tiles:
        f.write("    .byte " + ",".join("$%02X"%v for v in b) + "\n")
    f.write("theme_pals:\n")
    for pp in THEMES:
        f.write("    .word " + ",".join("$%04X"%v for v in pp) + "\n")
    f.write("piece_masks:\n")
    for i,p in enumerate(order):
        f.write("    .word " + ",".join("$%04X"%piece_masks[i*4+r] for r in range(4)) + "\n")
    f.write("piece_tiles:\n    .byte " + ",".join(str(t) for t in piece_tiles) + "\n")
    f.write("music_song:\n")
    for x,fr in music_song:
        f.write(f"    .word ${x:04X}\n    .byte {fr}\n")
print(f"wrote {OUT}: {len(tiles)} tiles, {NICONS} icons, {len(music_song)} notes")
