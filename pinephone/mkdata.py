#!/usr/bin/env python3
"""Generate pinephone/build/gfx.s from the shared UnoDOS assets (PinePhone / AArch64).

The Pi port draws into the firmware-allocated **mailbox framebuffer** — a flat
640x480, 32bpp XRGB8888 surface (depth 32, allocated at boot via the VideoCore
property channel). So, exactly like the GBA Mode-3 port, there are no hardware
tiles: we plot an 8x8 bitmap font and 16x16 icons pixel by pixel, each pixel's
palette INDEX looked up in a 16-entry 32-bit palette held in RAM (so the Theme
app recolours by swapping the table + redrawing).

This emits GNU-as (aarch64-linux-gnu-as) data: the font, the icons (index bytes),
the Dostris piece tables, the music tune (note Hz for the PWM tone path), and the
theme palettes (32-bit XRGB).

Usage: python pinephone/mkdata.py   (from the repo root)
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(HERE, "build", "gfx.s")

# ---- shared 8x8 font (1bpp; the same kernel/font8x8.asm every port uses) --------
font = []
for line in open(os.path.join(ROOT, "kernel", "font8x8.asm"), encoding="latin-1"):
    m = re.match(r"\s*db\s+0b([01]{8})", line)
    if m:
        font.append(int(m.group(1), 2))
assert len(font) == 95 * 8, f"font rows: {len(font)}"

# ---- icons from the x86 .BIN donors -> 16x16 palette-index maps -----------------
# donor chunky 0-3 -> palette index: 0->0 desktop, 1->2 cyan, 2->3 magenta, 3->1 white
icmap = {0: 0, 1: 2, 2: 3, 3: 1}
def icon_idx(path):
    data = open(path, "rb").read()
    assert data[0] == 0xEB and data[2:4] == b"UI", f"{path}: no UI header"
    chunky = data[0x10:0x50]
    out = []
    for r in range(16):
        rowb = chunky[r * 4:(r + 1) * 4]
        for px in range(16):
            out.append(icmap[(rowb[px // 4] >> ((3 - (px % 4)) * 2)) & 3])
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
icons = []
for name, binfile in ICONS:
    p = os.path.join(ROOT, binfile)
    if not os.path.exists(p):
        sys.exit(f"missing {binfile} - run 'make floppy144' first")
    icons.append(icon_idx(p))
paint = []
for row in PAINT_ICON:
    for ch in row:
        paint.append(icmap[int(ch)])
icons.append(paint)

# ---- Dostris tetromino tables (same Contract shape as every port) ---------------
PIECES = {
    "I": ["....", "####", "....", "...."], "O": [".##.", ".##.", "....", "...."],
    "T": [".#..", "###.", "....", "...."], "S": [".##.", "##..", "....", "...."],
    "Z": ["##..", ".##.", "....", "...."], "J": ["#...", "###.", "....", "...."],
    "L": ["..#.", "###.", "....", "...."]}
def rot90(g):
    return ["".join(g[3 - c][r] for c in range(4)) for r in range(4)]
def mask(g):
    m = 0
    for r in range(4):
        for c in range(4):
            if g[r][c] == "#":
                m |= 1 << (15 - (r * 4 + c))
    return m
order = ["I", "O", "T", "S", "Z", "J", "L"]
piece_masks = []
for p in order:
    g = PIECES[p]
    for _ in range(4):
        piece_masks.append(mask(g))
        g = rot90(g)
# piece -> palette index (block colour): I=cyan O=yellow T=magenta S=green Z=red J=white L=orange
piece_tiles = [2, 6, 3, 5, 7, 1, 8]

# ---- Music: a tone tune; on the Pi the value is the note FREQUENCY in Hz, fed to
#      the PWM (headphone-jack) tone path. The same 'Ode to Joy' as every port. ----
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
music_song = [(NOTE_HZ[n], d) for n, d in TUNE]

# ---- ST7703 (Xingbangda XBD599) MIPI-DSI panel init -----------------------------
# The PinePhone has NO bootloader stage that lights the DSI panel (unlike the Pi,
# where VideoCore firmware brings up HDMI), so the payload does the full bring-up
# itself. The panel controller needs ~20 DCS commands. Rather than compute the MIPI
# DSI packet framing (ECC over the header, CRC-16 over the payload) in assembly, we
# precompute each complete packet here and emit it as a byte blob the kernel just
# spills into the DSI TX FIFO. Command bytes are byte-identical to Apache NuttX's
# pinephone_lcd.c (lupyuen) / the Linux panel-sitronix-st7703 xbd599 sequence.
# Each entry: (full command array incl. the DCS command byte, delay-ms after sending).
ST7703 = [
    ([0xB9, 0xF1, 0x12, 0x83], 0),                                          # SETEXTC
    ([0xBA, 0x33, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x91, 0x0A, 0x00,
      0x00, 0x02, 0x4F, 0x11, 0x00, 0x00, 0x37], 0),                        # SETMIPI (4 lanes)
    ([0xB8, 0x25, 0x22, 0x20, 0x03], 0),                                    # SETPOWER_EXT
    ([0xB3, 0x10, 0x10, 0x05, 0x05, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00], 0),# SETRGBIF
    ([0xC0, 0x73, 0x73, 0x50, 0x50, 0x00, 0xC0, 0x08, 0x70, 0x00], 0),      # SETSCR
    ([0xBC, 0x4E], 0),                                                      # SETVDC
    ([0xCC, 0x0B], 0),                                                      # SETPANEL
    ([0xB4, 0x80], 0),                                                      # SETCYC
    ([0xB2, 0xF0, 0x12, 0xF0], 0),                                          # SETDISP (720x1440)
    ([0xE3, 0x00, 0x00, 0x0B, 0x0B, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
      0xFF, 0x00, 0xC0, 0x10], 0),                                          # SETEQ
    ([0xC6, 0x01, 0x00, 0xFF, 0xFF, 0x00], 0),                              # (undoc)
    ([0xC1, 0x74, 0x00, 0x32, 0x32, 0x77, 0xF1, 0xFF, 0xFF, 0xCC, 0xCC,
      0x77, 0x77], 0),                                                      # SETPOWER
    ([0xB5, 0x07, 0x07], 0),                                                # SETBGP
    ([0xB6, 0x2C, 0x2C], 0),                                                # SETVCOM
    ([0xBF, 0x02, 0x11, 0x00], 0),                                          # (undoc)
    ([0xE9, 0x82, 0x10, 0x06, 0x05, 0xA2, 0x0A, 0xA5, 0x12, 0x31, 0x23,
      0x37, 0x83, 0x04, 0xBC, 0x27, 0x38, 0x0C, 0x00, 0x03, 0x00, 0x00,
      0x00, 0x0C, 0x00, 0x03, 0x00, 0x00, 0x00, 0x75, 0x75, 0x31, 0x88,
      0x88, 0x88, 0x88, 0x88, 0x88, 0x13, 0x88, 0x64, 0x64, 0x20, 0x88,
      0x88, 0x88, 0x88, 0x88, 0x88, 0x02, 0x88, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00], 0),                                    # SETGIP1 (60B)
    ([0xEA, 0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x64,
      0x88, 0x13, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x75,
      0x88, 0x23, 0x14, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0A,
      0xA5, 0x00, 0x00, 0x00, 0x00], 0),                                    # SETGIP2 (60B)
    ([0xE0, 0x00, 0x09, 0x0D, 0x23, 0x27, 0x3C, 0x41, 0x35, 0x07, 0x0D,
      0x0E, 0x12, 0x13, 0x10, 0x12, 0x12, 0x18, 0x00, 0x09, 0x0D, 0x23,
      0x27, 0x3C, 0x41, 0x35, 0x07, 0x0D, 0x0E, 0x12, 0x13, 0x10, 0x12,
      0x12, 0x18], 0),                                                      # SETGAMMA (35B)
    ([0x11], 120),                                                          # SLPOUT, wait 120 ms
    ([0x29], 20),                                                           # DISPON
]


def mipi_ecc(b0, b1, b2):
    """MIPI DSI packet-header ECC: a Hamming code over the 24 header bits."""
    d = ([(b0 >> i) & 1 for i in range(8)] + [(b1 >> i) & 1 for i in range(8)] +
         [(b2 >> i) & 1 for i in range(8)])
    e = [0] * 8
    e[0] = d[0]^d[1]^d[2]^d[4]^d[5]^d[7]^d[10]^d[11]^d[13]^d[16]^d[20]^d[21]^d[22]^d[23]
    e[1] = d[0]^d[1]^d[3]^d[4]^d[6]^d[8]^d[10]^d[12]^d[14]^d[17]^d[20]^d[21]^d[22]^d[23]
    e[2] = d[0]^d[2]^d[3]^d[5]^d[6]^d[9]^d[11]^d[12]^d[15]^d[18]^d[20]^d[21]^d[22]
    e[3] = d[1]^d[2]^d[3]^d[7]^d[8]^d[9]^d[13]^d[14]^d[15]^d[19]^d[20]^d[21]^d[23]
    e[4] = d[4]^d[5]^d[6]^d[7]^d[8]^d[9]^d[16]^d[17]^d[18]^d[19]^d[20]^d[22]^d[23]
    e[5] = d[10]^d[11]^d[12]^d[13]^d[14]^d[15]^d[16]^d[17]^d[18]^d[19]^d[21]^d[22]^d[23]
    return sum(e[i] << i for i in range(8))


def mipi_crc(payload):
    """MIPI DSI long-packet CRC-16 (CCITT, reflected poly 0x8408, init 0xFFFF)."""
    crc = 0xFFFF
    for b in payload:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def dcs_packet(cmd):
    """Build the complete MIPI DSI packet for a DCS write (virtual channel 0).
    1 byte -> DCS short write (DT 0x05); 2 -> short write+param (0x15); >=3 -> long
    write (0x39, with payload + CRC-16 footer). Header is {DI, b0/wc_lo, b1/wc_hi, ECC}."""
    n = len(cmd)
    dt = 0x05 if n == 1 else (0x15 if n == 2 else 0x39)
    di = dt                                          # (vc=0 << 6) | dt
    if dt != 0x39:
        d0, d1 = cmd[0], (cmd[1] if n == 2 else 0)
        return [di, d0, d1, mipi_ecc(di, d0, d1)]
    wc = n
    crc = mipi_crc(cmd)
    return ([di, wc & 0xFF, (wc >> 8) & 0xFF, mipi_ecc(di, wc & 0xFF, (wc >> 8) & 0xFF)]
            + list(cmd) + [crc & 0xFF, (crc >> 8) & 0xFF])

# ---- palettes: 4 theme presets x 16 colours (32-bit XRGB 0xFFRRGGBB) ------------
def c(r, g, b):
    return (0xFF << 24) | (r << 16) | (g << 8) | b
#       0 desktop      1 white         2 cyan          3 magenta       4 black
BASE = [None,          c(255,255,255), c(0,230,230),   c(230,0,230),   c(0,0,0),
#       5 green        6 yellow        7 red           8 orange        9 grey
        c(0,220,0),    c(255,235,0),   c(235,0,0),     c(255,140,0),   c(128,128,128)]
BASE += [c(0,0,0)] * (16 - len(BASE))
NTHEMES = 4
DESKTOPS = [c(32,64,160), c(16,96,40), c(150,24,24), c(56,56,72)]   # blue green red grey
THEMES = []
for dk in DESKTOPS:
    pal = list(BASE)
    pal[0] = dk
    THEMES.append(pal)

# ---- assemble-time constants (kernel.s .includes these) -------------------------
EQU_OUT = os.path.join(HERE, "build", "gfxequ.inc")
with open(EQU_OUT, "w", newline="\n") as f:
    f.write("// AUTO-GENERATED by pinephone/mkdata.py - do not edit\n")
    f.write(".equ NICONS, %d\n" % NICONS)
    f.write(".equ NTHEMES, %d\n" % NTHEMES)
    f.write(".equ MUSIC_COUNT, %d\n" % len(music_song))

# ---- emit -----------------------------------------------------------------------
with open(OUT, "w", newline="\n") as f:
    f.write("// AUTO-GENERATED by pinephone/mkdata.py - do not edit\n")
    f.write(".section .rodata\n.align 3\n")
    # font: 95*8 bytes
    f.write(".global font_data\nfont_data:\n")
    for g in range(95):
        f.write("    .byte " + ",".join(str(b) for b in font[g*8:(g+1)*8]) + "\n")
    # icons: NICONS * 256 index bytes
    f.write(".global icon_data\nicon_data:\n")
    for name, idxs in zip([n for n, _ in ICONS] + ["paint"], icons):
        f.write(f"    // {name}\n")
        for r in range(16):
            f.write("    .byte " + ",".join(str(v) for v in idxs[r*16:(r+1)*16]) + "\n")
    # piece tables
    f.write(".global piece_masks\n.align 1\npiece_masks:\n")
    for i, p in enumerate(order):
        f.write("    .hword " + ",".join("0x%04X" % piece_masks[i*4+r] for r in range(4)) + "\n")
    f.write(".global piece_tiles\npiece_tiles:\n    .byte " +
            ",".join(str(t) for t in piece_tiles) + "\n")
    # music: freq (hword), frames (byte), pad (byte) -> 4 bytes per note
    f.write(".global music_song\n.align 2\nmusic_song:\n")
    for hz, fr in music_song:
        f.write(f"    .hword {hz}\n    .byte {fr}\n    .byte 0\n")
    # theme palettes: NTHEMES * 16 XRGB words
    f.write(".global theme_pals\n.align 2\ntheme_pals:\n")
    for ti, pal in enumerate(THEMES):
        f.write(f"    // theme {ti}\n    .word " + ",".join("0x%08X" % v for v in pal) + "\n")
    # ST7703 panel init: a self-describing blob the kernel walks at boot. Each entry =
    # .hword packet_len, .hword delay_ms, then packet_len bytes (the full DSI packet:
    # 4-byte header [+ payload + 2-byte CRC for long writes]). Terminated by len == 0.
    f.write(".global dsi_init_seq\n.align 2\ndsi_init_seq:\n")
    for cmd, delay in ST7703:
        pkt = dcs_packet(cmd)
        f.write(f"    .hword {len(pkt)}, {delay}   // cmd 0x{cmd[0]:02X}\n")
        f.write("    .byte " + ",".join("0x%02X" % b for b in pkt) + "\n")
    f.write("    .hword 0, 0\n")

print(f"wrote {OUT}: font 95, {NICONS} icons, {len(music_song)} notes, {NTHEMES} themes, "
      f"{len(ST7703)} panel cmds")
