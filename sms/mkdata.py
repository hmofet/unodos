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
] + [0] * 9

# ---------------- emit ------------------------------------------------------------
def tilebytes(b):
    return "\n".join("    db " + ",".join(f"${v:02X}" for v in b[i:i+16])
                     for i in range(0, len(b), 16))

NORMAL_FONT_BASE = 1
INV_FONT_BASE    = 96
T_SOLW           = 191
T_SOLC           = 192
T_SOLM           = 193
ICON_BASE        = 194

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by sms/mkdata.py - do not edit\n\n")
    f.write(f"NTILES        EQU {len(tiles)}\n")
    f.write(f"T_FONT        EQU {NORMAL_FONT_BASE}   ; 1..95 = ASCII 32..126\n")
    f.write(f"T_FONTINV     EQU {INV_FONT_BASE}  ; inverted font (title bar)\n")
    f.write(f"T_SOLW        EQU {T_SOLW}\n")
    f.write(f"T_SOLC        EQU {T_SOLC}\n")
    f.write(f"T_SOLM        EQU {T_SOLM}\n")
    f.write(f"T_ICONS       EQU {ICON_BASE}  ; 4 tiles per icon\n")
    f.write(f"NICONS        EQU {NICONS}\n\n")
    f.write("; 4bpp planar tile blob, uploaded contiguously to VRAM tile 0\n")
    f.write("tiles_all:\n")
    for comment, b in tiles:
        f.write(f"    ; tile - {comment}\n{tilebytes(b)}\n")
    f.write("\n; CRAM background palette (16 entries)\n")
    f.write("palette:\n    db " + ",".join(f"${v:02X}" for v in PALETTE) + "\n")

print(f"wrote {OUT}: {len(tiles)} tiles ({len(tiles)*32} bytes), {NICONS} icons")
