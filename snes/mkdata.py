#!/usr/bin/env python3
"""Generate snes/gen_data.inc from the shared UnoDOS assets (the SNES
twin of genesis/mkdata.py).

Emits:
  - tiles_bg: contiguous 4bpp planar BG tile blob, DMA'd to the BG1 char
    base (VRAM word $1000):
        0        blank (transparent index 0)
        1..95    font (ASCII 32-126): fg = index 1, bg = index 2 (opaque)
        96       solid cell (index 2 = the scheme background)
        97..99   window chrome edges: left, right, bottom (1px index-1 line)
        100..101 window chrome corners: BL, BR
  - tiles_spr: 4bpp cursor sprite tiles (the shared UnoDOS arrow, 8x16 as
    two 8x8 tiles), DMA'd to the OBJ char base.
  - pal_bg: four 16-colour BG palette lines (BGR555) = the four UnoDOS UI
    attribute schemes (PORT-SPEC SS1): NORM/INV/ACC/KEY. The same font and
    solid tiles serve every scheme; only the palette-select bits change.
  - pal_spr: the cursor sprite palette (index 1 white, 2 blue, 3 cyan).

SNES 4bpp planar tile = 32 bytes: eight (plane0,plane1) row pairs then
eight (plane2,plane3) pairs; pixel colour = b0 | b1<<1 | b2<<2 | b3<<3,
MSB = leftmost.

Usage: python snes/mkdata.py   (from the repo root)
"""
import re

OUT = "snes/gen_data.inc"

# ---------------- font (shared x86 8x8) ----------------
font = []
for line in open("kernel/font8x8.asm", encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"


def tile4bpp(rows):
    """rows: 8 lists of 8 palette indices (0..15) -> 32 planar bytes."""
    out = bytearray()
    for base in (0, 2):                  # planes (0,1) then (2,3)
        for r in rows:
            lo = hi = 0
            for x in range(8):
                bit = 7 - x
                lo |= ((r[x] >> base) & 1) << bit
                hi |= ((r[x] >> (base + 1)) & 1) << bit
            out.append(lo)
            out.append(hi)
    return out


# ---------------- BG tiles ----------------
bg = []   # (comment, 32 bytes)
bg.append(("blank", tile4bpp([[0] * 8 for _ in range(8)])))
for g in range(95):
    rows = []
    for r in font[g * 8:(g + 1) * 8]:
        rows.append([1 if r & (0x80 >> b) else 2 for b in range(8)])
    ch = chr(32 + g)
    bg.append(("'%s'" % (ch if ch not in "'" else "quote"), tile4bpp(rows)))
bg.append(("solid bg(2)", tile4bpp([[2] * 8 for _ in range(8)])))


def edge(l=False, r=False, b=False):
    rows = []
    for y in range(8):
        rows.append([1 if (l and x == 0) or (r and x == 7) or (b and y == 7)
                     else 2 for x in range(8)])
    return tile4bpp(rows)


bg.append(("edge left", edge(l=True)))
bg.append(("edge right", edge(r=True)))
bg.append(("edge bottom", edge(b=True)))
bg.append(("corner BL", edge(l=True, b=True)))
bg.append(("corner BR", edge(r=True, b=True)))

T_BLANK, T_FONT, T_SOLBG = 0, 1, 96
T_EDGEL, T_EDGER, T_EDGEB, T_CORNBL, T_CORNBR = 97, 98, 99, 100, 101
assert len(bg) == 102, len(bg)

# ---------------- cursor sprite (shared UnoDOS arrow, 8x16) ----------------
# (planeA, planeB) per row from the Amiga sprite; combo A|B<<1 -> sprite
# palette index: 1 white (outline), 2 blue (fill), 3 cyan.
arrow = [
    (0b10000000, 0b00000000), (0b11000000, 0b01000000),
    (0b11100000, 0b01100000), (0b11110000, 0b01110000),
    (0b11111000, 0b01111000), (0b11111100, 0b01111100),
    (0b11111110, 0b01111110), (0b11111111, 0b01111111),
    (0b11111100, 0b01111000), (0b11011000, 0b01001000),
    (0b10001100, 0b00000100), (0b00001100, 0b00000100),
    (0b00000110, 0b00000010), (0b00000110, 0b00000010),
    (0, 0), (0, 0)]
cmap = {0: 0, 1: 1, 2: 2, 3: 3}
crows = []
for a, b in arrow:
    crows.append([cmap[((a >> (7 - x)) & 1) | (((b >> (7 - x)) & 1) << 1)]
                  for x in range(8)])
spr = [("cursor top", tile4bpp(crows[:8])),
       ("cursor bottom", tile4bpp(crows[8:]))]

# ---------------- palettes (BGR555) ----------------
def bgr555(r, g, b):
    return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)

WHITE = (0xFF, 0xFF, 0xFF)
BLUE = (0x00, 0x00, 0xA8)
CYAN = (0x00, 0xA8, 0xA8)
MAG = (0xA8, 0x00, 0xA8)
BLACK = (0, 0, 0)

def palette(c0, c1, c2, c3, c4):
    return [bgr555(*c) for c in (c0, c1, c2, c3, c4)] + [0] * 11

# four UI schemes: index1 = fg, index2 = bg
pal_bg = (palette(BLUE, WHITE, BLUE, CYAN, MAG)    # 0 NORM white-on-blue
          + palette(BLUE, BLUE, WHITE, CYAN, MAG)  # 1 INV  blue-on-white
          + palette(BLUE, CYAN, BLUE, WHITE, MAG)  # 2 ACC  cyan-on-blue
          + palette(BLUE, BLUE, CYAN, WHITE, MAG)) # 3 KEY  blue-on-cyan

pal_spr = [0, bgr555(*WHITE), bgr555(*BLUE), bgr555(*CYAN)] + [0] * 12

# ---------------- emit ----------------
def byte_rows(data):
    return "\n".join("    .byte " + ",".join(f"${b:02X}" for b in data[i:i + 16])
                     for i in range(0, len(data), 16))

def words(ws):
    return "\n".join("    .word " + ",".join(f"${w:04X}" for w in ws[i:i + 16])
                     for i in range(0, len(ws), 16))

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by snes/mkdata.py - do not edit\n\n")
    f.write(f"NBGTILES    = {len(bg)}\n")
    f.write(f"BGTILE_BYTES = {len(bg) * 32}\n")
    f.write(f"SPRTILE_BYTES = {len(spr) * 32}\n")
    f.write(f"T_BLANK   = {T_BLANK}\n")
    f.write(f"T_FONT    = {T_FONT}\n")
    f.write(f"T_SOLBG   = {T_SOLBG}\n")
    f.write(f"T_EDGEL   = {T_EDGEL}\n")
    f.write(f"T_EDGER   = {T_EDGER}\n")
    f.write(f"T_EDGEB   = {T_EDGEB}\n")
    f.write(f"T_CORNBL  = {T_CORNBL}\n")
    f.write(f"T_CORNBR  = {T_CORNBR}\n\n")
    f.write("; BG 4bpp tile blob -> BG1 char base (VRAM word $1000)\n")
    f.write("tiles_bg:\n")
    for comment, data in bg:
        f.write(f"  ; {comment}\n" + byte_rows(data) + "\n")
    f.write("\n; cursor sprite tiles -> OBJ char base (VRAM word $4000)\n")
    f.write("tiles_spr:\n")
    for comment, data in spr:
        f.write(f"  ; {comment}\n" + byte_rows(data) + "\n")
    f.write("\n; four BG palette lines (BGR555): NORM, INV, ACC, KEY\n")
    f.write("pal_bg:\n" + words(pal_bg) + "\n")
    f.write("\n; cursor sprite palette (BGR555)\n")
    f.write("pal_spr:\n" + words(pal_spr) + "\n")

print(f"wrote {OUT}: {len(bg)} BG tiles, {len(spr)} sprite tiles, "
      f"{len(pal_bg)} BG + {len(pal_spr)} sprite palette entries")
