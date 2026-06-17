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

assert len(tiles) <= 256, f"{len(tiles)} tiles exceeds pattern table 0"

# ---- palette (NES master-palette indices) --------------------------------------
PAL = [0x11, 0x30, 0x2C, 0x24]   # blue(backdrop), white, cyan, magenta

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
with open(INC_OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by nes/mkdata.py - do not edit\n")
    f.write(f"T_FONT      EQU {FONT_BASE}   ; 1..95 = ASCII 32..126 (white on blue)\n")
    f.write(f"T_FONTINV   EQU {INV_BASE}  ; inverted font (blue on white) - title bar\n")
    f.write(f"T_WHITE     EQU {WHITE_TILE}  ; white block (title-bar bg)\n")
    f.write(f"T_ICONS     EQU {ICON_BASE}  ; 4 tiles per icon\n")
    f.write(f"NICONS      EQU {NICONS}\n")
    f.write("palette:\n    dc.b " + ",".join(f"${v:02X}" for v in PAL) + "\n")

print(f"wrote {CHR_OUT} (8KB) + {INC_OUT}: {len(tiles)} tiles, {NICONS} icons")
