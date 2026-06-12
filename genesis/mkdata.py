#!/usr/bin/env python3
"""Generate genesis/gen_data.i from the shared UnoDOS assets.

- tiles_all: one contiguous 4bpp tile blob loaded at VRAM tile 1:
    1..95   font (ASCII 32-126), fg = index 1, bg = index 2 (opaque)
    96..99  solids: bg(2), fg(1), cyan(3), magenta(4)
    100..105 window chrome: edge L/R/B, corners BL/BR, staff hline
    106..107 mouse cursor sprite (8x16, the shared UnoDOS arrow)
    108..123 icons: sysinfo, clock, notepad, music (4 tiles each,
             pulled from the x86 .BIN headers, 16x16 2bpp chunky)
- mus table: Canon in D (same arrangement as apps/music.asm) as PSG
  tone register values (NTSC) + durations in 60Hz vblank ticks +
  staff y-offsets in pixels.
- ps2map/ps2map_sh: PS/2 scancode set 2 -> ASCII (US layout).

Usage: python genesis/mkdata.py   (from the repo root)
"""
import re, sys, os

OUT = "genesis/gen_data.i"

# ---------------- font (shared x86 8x8) ----------------
font = []
for line in open("kernel/font8x8.asm", encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

def tile_from_rows(pixrows):
    """pixrows: 8 lists of 8 palette indices -> 8 longwords (4bpp)."""
    longs = []
    for row in pixrows:
        v = 0
        for px in row:
            v = (v << 4) | (px & 15)
        longs.append(v)
    return longs

tiles = []   # list of (comment, [8 longs])

# font: fg=1 on bg=2 (opaque text cell)
for g in range(95):
    rows = []
    for r in font[g * 8:(g + 1) * 8]:
        rows.append([1 if r & (0x80 >> b) else 2 for b in range(8)])
    ch = chr(32 + g)
    tiles.append(("'%s'" % (ch if ch != "'" else "quote"), tile_from_rows(rows)))

# solids
for name, idx in [("solid bg(2)", 2), ("solid fg(1)", 1),
                  ("solid cyan(3)", 3), ("solid magenta(4)", 4)]:
    tiles.append((name, tile_from_rows([[idx] * 8] * 8)))

# window chrome on bg=2: 1px line of fg=1
def edge(l=False, r=False, b=False):
    rows = []
    for y in range(8):
        row = []
        for x in range(8):
            on = (l and x == 0) or (r and x == 7) or (b and y == 7)
            row.append(1 if on else 2)
        rows.append(row)
    return tile_from_rows(rows)

tiles.append(("edge left", edge(l=True)))
tiles.append(("edge right", edge(r=True)))
tiles.append(("edge bottom", edge(b=True)))
tiles.append(("corner BL", edge(l=True, b=True)))
tiles.append(("corner BR", edge(r=True, b=True)))
# staff hline: 1px fg line on bg, row 3
tiles.append(("staff hline", tile_from_rows(
    [[1 if y == 3 else 2 for _ in range(8)] for y in range(8)])))

# ---------------- cursor sprite (shared UnoDOS arrow, 8x16) ----------------
# Amiga sprite rows: (planeA, planeB) words; high byte = the 8px we use.
# combo A=1,B=0 -> 15 (white), B=1,A=0 -> 14 (blue), both -> 13 (cyan).
arrow = [
    (0b10000000, 0b00000000), (0b11000000, 0b01000000),
    (0b11100000, 0b01100000), (0b11110000, 0b01110000),
    (0b11111000, 0b01111000), (0b11111100, 0b01111100),
    (0b11111110, 0b01111110), (0b11111111, 0b01111111),
    (0b11111100, 0b01111000), (0b11011000, 0b01001000),
    (0b10001100, 0b00000100), (0b00001100, 0b00000100),
    (0b00000110, 0b00000010), (0b00000110, 0b00000010),
    (0, 0), (0, 0)]
cmap = {0: 0, 1: 15, 2: 14, 3: 13}
crows = []
for a, b in arrow:
    crows.append([cmap[((a >> (7 - x)) & 1) | (((b >> (7 - x)) & 1) << 1)]
                  for x in range(8)])
tiles.append(("cursor top", tile_from_rows(crows[:8])))
tiles.append(("cursor bottom", tile_from_rows(crows[8:])))

# ---------------- icons from the x86 .BIN headers ----------------
# 2bpp chunky values 0-3 = UI colors 0-3 -> PAL0 indices 2,3,4,1
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
            out.append(tile_from_rows(
                [pix[ty + y][tx:tx + 8] for y in range(8)]))
    return out

for name, binfile in [("sysinfo", "build/sysinfo.bin"),
                      ("clock",   "build/clock.bin"),
                      ("notepad", "build/notepad.bin"),
                      ("music",   "build/music.bin"),
                      ("dostris", "build/tetris.bin"),
                      ("outlast", "build/outlast.bin"),
                      ("pacman",  "build/pacman.bin"),
                      ("files",   "build/browser.bin"),
                      ("theme",   "build/settings.bin"),
                      ("tracker", "build/music.bin")]:
    if not os.path.exists(binfile):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    for i, t in enumerate(icon_tiles(binfile)):
        tiles.append((f"icon {name} {i}", t))

# paint icon: synthesized (no x86 donor yet) - brush over a daub,
# same 2bpp chunky semantics as the .BIN headers (icmap indexes)
PAINT_ICON = [
    "0000000000033000",
    "0000000000333000",
    "0000000003330000",
    "0000000033300000",
    "0000000333000000",
    "0000011330000000",
    "0000111100000000",
    "0000111000000000",
    "0001110000000000",
    "0001100000000000",
    "0022000022220000",
    "0222200222222000",
    "2222222222222200",
    "2222222222222220",
    "0222222222222200",
    "0002222222220000",
]
pix = [[icmap[int(c)] for c in row] for row in PAINT_ICON]
for ty in (0, 8):
    for tx in (0, 8):
        tiles.append((f"icon paint {ty//8*2+tx//8}", tile_from_rows(
            [pix[ty + y][tx:tx + 8] for y in range(8)])))

# ---------------- game tiles ----------------
# solids for palette indexes 5..15 (game colors live in PAL2/PAL3 5-15)
for idx in range(5, 16):
    tiles.append((f"solid {idx}", tile_from_rows([[idx] * 8] * 8)))

# Pac-Man maze cell tiles (PAL2: 10 = wall blue, 14 = white, 15 = black,
# 4 = magenta is NOT in PAL2 - gate uses 13 pink)
def pm_tile(fn):
    return tile_from_rows([[fn(x, y) for x in range(8)] for y in range(8)])

tiles.append(("pm wall", pm_tile(
    lambda x, y: 10 if 0 < x < 7 and 0 < y < 7 else 15)))
tiles.append(("pm dot", pm_tile(
    lambda x, y: 14 if 3 <= x <= 4 and 3 <= y <= 4 else 15)))
tiles.append(("pm power", pm_tile(
    lambda x, y: 14 if 2 <= x <= 5 and 2 <= y <= 5 else 15)))
tiles.append(("pm gate", pm_tile(
    lambda x, y: 13 if 3 <= y <= 4 else 15)))

# actor sprite tiles: pac (yellow 6), ghosts (body color + white eyes),
# frightened (PAL3: 13 = fright blue, 15 = black eyes), eaten (white core)
tiles.append(("spr pac", pm_tile(
    lambda x, y: 6 if 1 <= x <= 6 and 1 <= y <= 6 else 0)))
def ghost(body, eye):
    return pm_tile(lambda x, y: eye if y == 2 and x in (2, 5)
                   else (body if 1 <= x <= 6 and 1 <= y <= 6 else 0))
tiles.append(("spr ghost red", ghost(12, 14)))
tiles.append(("spr ghost pink", ghost(13, 14)))
tiles.append(("spr ghost orange", ghost(11, 14)))
tiles.append(("spr ghost fright", ghost(13, 15)))
tiles.append(("spr ghost eaten", pm_tile(
    lambda x, y: 14 if 3 <= x <= 5 and 3 <= y <= 5 else 0)))

# ---------------- music: Canon in D (PSG, NTSC) ----------------
F = {"C4": 262, "D4": 294, "E4": 330, "F4": 349, "G4": 392, "A4": 440,
     "B4": 494, "C5": 523, "D5": 587, "E5": 659, "F5": 698, "G5": 784}
M = {"C4": 60, "D4": 62, "E4": 64, "F4": 65, "G4": 67, "A4": 69, "B4": 71,
     "C5": 72, "D5": 74, "E5": 76, "F5": 77, "G5": 79}
QN, EN = 30, 16                       # quarter/eighth in 60Hz ticks
tune = ([("C5", QN), ("B4", QN), ("A4", QN), ("G4", QN),
         ("F4", QN), ("E4", QN), ("F4", QN), ("G4", QN)]
        + [(n, EN) for n in ["C5", "E5", "B4", "D5", "A4", "C5", "G4", "B4",
                             "F4", "A4", "E4", "G4", "F4", "A4", "G4", "B4"]])
PSG_CLK = 3579545
notes = [(round(PSG_CLK / (32 * F[n])), d, (M[n] - 60) * 2) for n, d in tune]

# ---------------- game songs (parsed from the x86 sources) ----------------
def parse_song(path, label):
    src = open(path, encoding="latin-1").read()
    equs = {m.group(1): int(m.group(2)) for m in
            re.finditer(r"^(\w+)\s+equ\s+(\d+)", src, re.M)}
    block = src.split(label + ":", 1)[1]
    out = []
    for m in re.finditer(r"dw\s+(\w+),\s*(\w+)", block):
        f, d = m.group(1), m.group(2)
        fv = equs.get(f, int(f, 0) if f[0].isdigit() else 0)
        dv = equs.get(d, int(d, 0) if d[0].isdigit() else 0)
        if fv == 0xFFFF:
            break
        out.append((fv, dv))
    return out

def song_psg(song):
    # (freq Hz, dur 18.2Hz ticks) -> (PSG tone value or 0, dur 60Hz ticks)
    return [(round(PSG_CLK / (32 * f)) if f else 0,
             max(1, round(d * 33 / 10))) for f, d in song]

koro = song_psg(parse_song("apps/tetris.asm", "korobeiniki"))
drive = song_psg(parse_song("apps/outlast.asm", "song_game1"))

# ---------------- PS/2 scancode set 2 -> ASCII ----------------
s2 = {}
for code, ch in zip([0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43,
                     0x3B, 0x42, 0x4B, 0x3A, 0x31, 0x44, 0x4D, 0x15, 0x2D,
                     0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A],
                    "abcdefghijklmnopqrstuvwxyz"):
    s2[code] = ch
for code, ch in zip([0x45, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E,
                     0x46], "0123456789"):
    s2[code] = ch
for code, ch in [(0x0E, '`'), (0x4E, '-'), (0x55, '='), (0x5D, '\\'),
                 (0x54, '['), (0x5B, ']'), (0x4C, ';'), (0x52, "'"),
                 (0x41, ','), (0x49, '.'), (0x4A, '/'), (0x29, ' ')]:
    s2[code] = ch
s2[0x5A] = '\r'   # Enter -> 13
s2[0x66] = '\x08'  # Backspace
s2[0x0D] = '\t'
s2[0x76] = '\x1b'  # Esc
SHMAP = {"`": "~", "1": "!", "2": "@", "3": "#", "4": "$", "5": "%",
         "6": "^", "7": "&", "8": "*", "9": "(", "0": ")", "-": "_",
         "=": "+", "[": "{", "]": "}", "\\": "|", ";": ":", "'": '"',
         ",": "<", ".": ">", "/": "?"}
ps2map, ps2map_sh = [0] * 132, [0] * 132
for code, ch in s2.items():
    ps2map[code] = ord(ch)
    if "a" <= ch <= "z":
        ps2map_sh[code] = ord(ch.upper())
    else:
        ps2map_sh[code] = ord(SHMAP.get(ch, ch))

# ---------------- emit ----------------
def bytes_(vals):
    return "\n".join("    dc.b " + ",".join(f"${v:02X}" for v in vals[i:i+16])
                     for i in range(0, len(vals), 16))

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by genesis/mkdata.py - do not edit\n\n")
    f.write("; 4bpp tile blob, loaded contiguously at VRAM tile 1\n")
    f.write(f"NTILES equ {len(tiles)}\n")
    f.write("tiles_all:\n")
    for comment, longs in tiles:
        f.write("    dc.l " + ",".join(f"${v:08X}" for v in longs)
                + f"   ; {comment}\n")
    f.write("\n; Canon in D: PSG tone value (NTSC), 60Hz ticks, staff y-off\n")
    f.write(f"mus_count: dc.w {len(notes)}\n")
    f.write("mus_notes:\n")
    for p, d, y in notes:
        f.write(f"    dc.w {p},{d},{y}\n")
    f.write("\n; game songs: (PSG tone value or 0, 60Hz ticks) pairs\n")
    f.write(f"koro_count: dc.w {len(koro)}\n")
    f.write("koro_notes:\n")
    for p, d in koro:
        f.write(f"    dc.w {p},{d}\n")
    f.write(f"drive_count: dc.w {len(drive)}\n")
    f.write("drive_notes:\n")
    for p, d in drive:
        f.write(f"    dc.w {p},{d}\n")
    f.write("\n; PS/2 scancode set 2 -> ASCII (US layout)\n")
    f.write("ps2map:\n" + bytes_(ps2map) + "\n")
    f.write("ps2map_sh:\n" + bytes_(ps2map_sh) + "\n")

print(f"wrote {OUT}: {len(tiles)} tiles, {len(notes)} notes, ps2 keymaps")
