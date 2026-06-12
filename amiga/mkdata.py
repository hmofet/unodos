#!/usr/bin/env python3
"""Generate amiga/gen_data.i from the x86 tree's assets.

- font8x8: parsed from kernel/font8x8.asm (95 glyphs, ASCII 32-126,
  1 byte/row, bit 7 = leftmost) -- exported byte-identical.
- icons: 16x16 2bpp chunky icons pulled from app .BIN headers
  (offset 0x10, 64 bytes) and converted to 2 separate bitplanes
  (16 words plane0, then 16 words plane1).
- keymap: Amiga rawkey -> ASCII table (128 bytes, US layout, no shift).
- romdisk: files from amiga/disk/ baked in as an editable-in-RAM file
  table (until the MFM/FAT12 driver lands).
- music: Canon in D note table (same arrangement as apps/music.asm) as
  Paula periods (PAL) + durations in vblank ticks + staff y-offsets.

Usage: python mkdata.py <out.i>   (run from the repo root)
"""
import re, sys, os, glob

OUT = sys.argv[1] if len(sys.argv) > 1 else "amiga/gen_data.i"

# ---------------- font ----------------
font = []
for line in open("kernel/font8x8.asm", encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

# ---------------- icons ----------------
def icon_planar(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    p0rows, p1rows = [], []
    for r in range(16):
        row = chunky[r*4:(r+1)*4]
        p0 = p1 = 0
        for px in range(16):
            byte = row[px // 4]
            shift = (3 - (px % 4)) * 2
            val = (byte >> shift) & 3
            bit = 15 - px
            p0 |= (val & 1) << bit
            p1 |= ((val >> 1) & 1) << bit
        p0rows.append(p0)
        p1rows.append(p1)
    return p0rows + p1rows

icons = {}
for name, binfile in [("icon_sysinfo", "build/sysinfo.bin"),
                      ("icon_clock",   "build/clock.bin"),
                      ("icon_files",   "build/browser.bin"),
                      ("icon_notepad", "build/notepad.bin"),
                      ("icon_music",   "build/music.bin"),
                      ("icon_theme",   "build/settings.bin"),
                      ("icon_dostris", "build/tetris.bin"),
                      ("icon_outlast", "build/outlast.bin"),
                      ("icon_pacman",  "build/pacman.bin"),
                      ("icon_tracker", "build/music.bin")]:
    if os.path.exists(binfile):
        icons[name] = icon_planar(binfile)
    else:
        sys.exit(f"missing {binfile} - run 'make floppy144' first")

# paint icon: synthesized (no x86 donor) - a brush over a paint daub,
# 2bpp chunky like the .BIN headers (0 bg, 1 cyan, 2 magenta, 3 white)
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
def icon_from_text(rows16):
    p0rows, p1rows = [], []
    for r in rows16:
        p0 = p1 = 0
        for px in range(16):
            val = int(r[px])
            bit = 15 - px
            p0 |= (val & 1) << bit
            p1 |= ((val >> 1) & 1) << bit
        p0rows.append(p0)
        p1rows.append(p1)
    return p0rows + p1rows
icons["icon_paint"] = icon_from_text(PAINT_ICON)

# ---------------- keymap ----------------
keymap = [0] * 128
def setk(code, ch): keymap[code] = ord(ch)
for i, ch in enumerate("1234567890-="):  setk(0x01 + i, ch)
for i, ch in enumerate("qwertyuiop"):    setk(0x10 + i, ch)
setk(0x1A, '['); setk(0x1B, ']')
for i, ch in enumerate("asdfghjkl"):     setk(0x20 + i, ch)
setk(0x29, ';'); setk(0x2A, "'")
for i, ch in enumerate("zxcvbnm,./"):    setk(0x31 + i, ch)
setk(0x40, ' ')
keymap[0x44] = 13    # Return
keymap[0x45] = 27    # Esc
keymap[0x41] = 8     # Backspace
keymap[0x42] = 9     # Tab
keymap[0x46] = 127   # Del

# ---------------- ROM-disk ----------------
diskdir = os.path.join(os.path.dirname(OUT) or ".", "disk")
rdfiles = sorted(glob.glob(os.path.join(diskdir, "*.TXT")))
romdisk = []
for f in rdfiles[:8]:
    data = open(f, "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r")
    cap = max(1024, (len(data) + 255) & ~255)
    name = os.path.basename(f).upper()[:12]
    romdisk.append((name, data, cap))

# ---------------- music: Canon in D ----------------
F = {"C4":262,"D4":294,"E4":330,"F4":349,"G4":392,"A4":440,"B4":494,
     "C5":523,"D5":587,"E5":659,"F5":698,"G5":784}
M = {"C4":60,"D4":62,"E4":64,"F4":65,"G4":67,"A4":69,"B4":71,
     "C5":72,"D5":74,"E5":76,"F5":77,"G5":79}
QN, EN = 25, 13                       # quarter/eighth in PAL vblank ticks
tune = ([("C5",QN),("B4",QN),("A4",QN),("G4",QN),
         ("F4",QN),("E4",QN),("F4",QN),("G4",QN)]
        + [(n,EN) for n in ["C5","E5","B4","D5","A4","C5","G4","B4",
                            "F4","A4","E4","G4","F4","A4","G4","B4"]])
PAL_CLK = 3546895
notes = []
for n, d in tune:
    period = round(PAL_CLK / (F[n] * 8))          # 8-sample square wave
    yoff = (M[n] - 60) * 2
    notes.append((period, d, yoff))


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

def song_amiga(song):
    # (freq Hz, dur 18.2Hz ticks) -> (Paula period or 0, dur PAL ticks)
    out = []
    for f, d in song:
        period = round(PAL_CLK / (f * 8)) if f else 0
        out.append((period, max(1, round(d * 11 / 4))))
    return out

koro  = song_amiga(parse_song("apps/tetris.asm", "korobeiniki"))
drive = song_amiga(parse_song("apps/outlast.asm", "song_game1"))

# ---------------- emit ----------------
def words(vals):
    out = []
    for i in range(0, len(vals), 8):
        out.append("    dc.w " + ",".join(f"${v:04X}" for v in vals[i:i+8]))
    return "\n".join(out)

def bytes_(vals):
    out = []
    for i in range(0, len(vals), 16):
        out.append("    dc.b " + ",".join(f"${v:02X}" for v in vals[i:i+16]))
    return "\n".join(out)

with open(OUT, "w", newline="\n") as f:
    f.write("; AUTO-GENERATED by amiga/mkdata.py - do not edit\n\n")
    f.write("; 8x8 font, ASCII 32-126, 1 byte/row, bit7 = leftmost pixel\n")
    f.write("font8x8:\n" + bytes_(font) + "\n\n")
    for name, rows in icons.items():
        f.write(f"; {name}: 16x16, 16 words plane0 then 16 words plane1\n")
        f.write(f"{name}:\n" + words(rows) + "\n\n")
    f.write("; Amiga rawkey -> ASCII (US layout, no shift)\n")
    f.write("keymap_ascii:\n" + bytes_(keymap) + "\n\n")

    # ROM-disk: entry = 12-byte name, dc.l data, dc.w size, dc.w capacity
    f.write("; ROM-disk: files baked into the image, editable in RAM\n")
    f.write(f"romdisk_count: dc.w {len(romdisk)}\n")
    f.write("romdisk_tab:\n")
    for i, (name, data, cap) in enumerate(romdisk):
        nb = name.encode("ascii").ljust(12, b"\x00")
        f.write("    dc.b " + ",".join(f"${b:02X}" for b in nb) + f"  ; {name}\n")
        f.write(f"    dc.l rdfile{i}\n    dc.w {len(data)},{cap}\n")
    f.write("\n")
    for i, (name, data, cap) in enumerate(romdisk):
        f.write(f"rdfile{i}:\n" + bytes_(list(data)) + "\n")
        f.write(f"    ds.b {cap - len(data)}\n")
    f.write("\n")

    # music table: dc.w period, durTicks, staffYoff; terminated by count
    f.write("; Korobeiniki + Sunset Drive (game music, Paula)\n")
    f.write("koro_count: dc.w %d\n" % len(koro))
    f.write("koro_notes:\n")
    f.write(words([v for pair in koro for v in pair]) + "\n")
    f.write("drive_count: dc.w %d\n" % len(drive))
    f.write("drive_notes:\n")
    f.write(words([v for pair in drive for v in pair]) + "\n")
    f.write("; Canon in D (same arrangement as apps/music.asm), Paula PAL periods\n")
    f.write(f"mus_count: dc.w {len(notes)}\n")
    f.write("mus_notes:\n")
    for p, d, y in notes:
        f.write(f"    dc.w {p},{d},{y}\n")

print(f"wrote {OUT}: font 760B, {len(icons)} icons, keymap, "
      f"{len(romdisk)} romdisk files, {len(notes)} notes")
