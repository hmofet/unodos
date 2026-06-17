#!/usr/bin/env python3
"""ROM-free Bandai WonderSwan test harness (Unicorn x86-16 / V30MZ core).

The WonderSwan CPU is an NEC V30MZ — an 80186-class x86 — so, exactly like the
GBA port runs its real ARM ROM on a Unicorn ARM7TDMI and the MacPlus port uses a
Unicorn 68K, this verifies the port headlessly by running the REAL WonderSwan ROM
on a Unicorn x86 core in 16-bit real mode. It:

  * maps the 16 KB internal RAM at 0x00000 and the cartridge ROM at the TOP of the
    1 MB space (so the V30MZ reset vector 0xFFFF0 lands in the ROM, exercising the
    genuine JMP-FAR boot path),
  * services the I/O ports the kernel touches: it records every OUT (display
    control, the SCR1 map-base, the scroll regs, the mono shade pool + palettes,
    sound) and answers IN of the line-counter (port 0x03) with a toggling scanline
    so `wait_vblank` advances, and the keypad (port 0xB5) per the held-key set,
  * then renders the SCR1 tilemap exactly as the LCD would: 28x18 visible tiles of
    8x8 2bpp planar patterns from RAM 0x2000, each pixel's 2bpp colour resolved
    through its tile's palette -> shade-pool -> 16-level grey, to a 224x144 PNG.

The AUTOTEST ROMs drive the keypad themselves, so for those the harness only has
to tick the line counter; nothing about the picture is faked.

Usage: python ws/harness.py <rom.ws> <out.png> [instr_millions]
"""
import sys, struct, zlib
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_PROT_ALL, UC_HOOK_INSN
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_SS,
                               UC_X86_REG_SP, UC_X86_INS_OUT, UC_X86_INS_IN)

RAM_BASE = 0x00000
RAM_SIZE = 0x20000            # 128 KB window covering the 16 KB internal RAM + low area
ROM_SIZE = 0x10000            # 64 KB ROM mapped at 0xF0000..0xFFFFF (top byte at 0xFFFFF)
ROM_BASE = 0x100000 - ROM_SIZE

TILE_BASE = 0x2000           # mono tile patterns live here in internal RAM
VIS_COLS, VIS_ROWS = 28, 18  # 224x144 / 8

io = {}                      # last value written to each I/O port
held = 0                     # active-high keypad bits for the real-input path (unused by AUTOTEST)


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    rom_path, out_path = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 8_000_000

    data = open(rom_path, "rb").read()
    if len(data) > ROM_SIZE:
        data = data[-ROM_SIZE:]                       # keep the top (where the header is)
    rom = bytearray(ROM_SIZE)
    rom[ROM_SIZE - len(data):] = data                 # ROM image sits flush against 0xFFFFF

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mem_map(ROM_BASE, ROM_SIZE, UC_PROT_ALL)
    uc.mem_write(ROM_BASE, bytes(rom))

    line = [0]

    def on_out(uc, port, size, value, ud):
        io[port & 0xFFFF] = value & 0xFF
        if size >= 2:
            io[(port & 0xFFFF) + 1] = (value >> 8) & 0xFF
        return 0

    def on_in(uc, port, size, ud):
        p = port & 0xFFFF
        if p == 0x03:                                  # LINE_CUR — toggle across the vblank line (144)
            line[0] ^= 1
            return 80 if line[0] else 160
        if p == 0xB5:                                  # KEYPAD: low nibble = selected group's keys
            sel = io.get(0xB5, 0)
            v = 0
            if sel & 0x20:                             # X group (the d-pad in horizontal layout)
                v = held & 0x0F
            elif sel & 0x40:                           # buttons group (A/B/Start)
                v = (held >> 4) & 0x0F
            return (sel & 0x70) | v
        return 0

    uc.hook_add(UC_HOOK_INSN, on_out, None, 1, 0, UC_X86_INS_OUT)
    uc.hook_add(UC_HOOK_INSN, on_in, None, 1, 0, UC_X86_INS_IN)

    # Reset state: CS=0xFFFF IP=0 -> linear 0xFFFF0 (the V30MZ reset vector inside ROM).
    uc.reg_write(UC_X86_REG_CS, 0xFFFF)
    uc.reg_write(UC_X86_REG_IP, 0x0000)
    uc.reg_write(UC_X86_REG_SS, 0x0000)
    uc.reg_write(UC_X86_REG_SP, 0x2000)

    CHUNK = 20000
    start = 0xFFFF0
    chunks = max(1, budget // CHUNK)
    for i in range(chunks):
        try:
            uc.emu_start(start, 0x100000, count=CHUNK)
        except Exception as e:
            print("  (stopped at chunk %d: %s)" % (i, e))
            break
        cs = uc.reg_read(UC_X86_REG_CS)
        ip = uc.reg_read(UC_X86_REG_IP)
        start = ((cs << 4) + ip) & 0xFFFFF

    render(uc, out_path)
    print("wrote %s (224x144) after ~%d instrs; display=%02X mapbase=%02X" %
          (out_path, chunks * CHUNK, io.get(0x00, 0), io.get(0x07, 0)))


def shade_pool():
    """8 shade-pool entries from ports 0x1C-0x1F (2 nibbles each), each a 0-15 grey."""
    pool = []
    for port in (0x1C, 0x1D, 0x1E, 0x1F):
        b = io.get(port, 0)
        pool.append(b & 0x0F)
        pool.append((b >> 4) & 0x0F)
    return pool


def pal_shade(pool, pal, color):
    """Resolve (palette, 2bpp colour) -> grey 0..255 through the WS mono indirection."""
    base = 0x20 + pal * 2 + (color >> 1)
    b = io.get(base, 0)
    nib = (b >> 4) if (color & 1) else (b & 0x0F)
    shade = pool[nib & 7]
    return 255 - shade * 17


def render(uc, out_path):
    W, H = VIS_COLS * 8, VIS_ROWS * 8
    rgb = bytearray(W * H * 3)
    ram = uc.mem_read(RAM_BASE, 0x4000)
    disp_on = io.get(0x00, 0) & 0x01
    map_base = (io.get(0x07, 0) & 0x07) << 11
    sx, sy = io.get(0x10, 0), io.get(0x11, 0)
    pool = shade_pool()

    for ty in range(VIS_ROWS):
        for tx in range(VIS_COLS):
            mcol = ((sx >> 3) + tx) & 31
            mrow = ((sy >> 3) + ty) & 31
            ent = map_base + (mrow * 32 + mcol) * 2
            entry = ram[ent] | (ram[ent + 1] << 8)
            tile = entry & 0x1FF
            pal = (entry >> 9) & 0x0F
            hflip = (entry >> 14) & 1
            vflip = (entry >> 15) & 1
            patt = TILE_BASE + tile * 16
            for row in range(8):
                pr = (7 - row) if vflip else row
                p0 = ram[patt + pr * 2]
                p1 = ram[patt + pr * 2 + 1]
                py = ty * 8 + row
                for col in range(8):
                    bit = col if hflip else (7 - col)
                    color = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1)
                    g = pal_shade(pool, pal, color) if disp_on else 0xC0
                    px = tx * 8 + col
                    o = (py * W + px) * 3
                    rgb[o] = rgb[o + 1] = rgb[o + 2] = g
    write_png(out_path, W, H, rgb)


if __name__ == "__main__":
    main()
