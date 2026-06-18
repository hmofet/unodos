#!/usr/bin/env python3
"""ROM-free NEC PC Engine test harness (HuC6280 core over py65 + a VDC model).

  ###########################################################################
  ## SUPERSEDED by Mednafen for this port (see C:\\Users\\arin\\mednafen-dl). ##
  ###########################################################################
  This py65-based core CANNOT model the real HuC6280, which hard-maps the zero
  page to logical $2000 and the stack to $2100. py65's 6502 core hardwires those
  to $0000/$0100 and the relocation lives in the CPU's addressing-mode logic, not
  the memory bus, so a py65 memory model can't intercept it (zp `lda $00` and
  absolute `lda $0000` both arrive as mem[0x0000] — indistinguishable). That is
  precisely the bug that rendered fine here but black-screened on real hardware:
  the kernel now maps RAM at MPR1 ($2000) and I/O at MPR0 ($0000), and forces the
  VDC ($0000-$0003) accesses to absolute with `a:` so ca65 doesn't fold them to
  zero page. None of that is observable on this core. The port is now verified on
  Mednafen (gold-standard, real-hardware-accurate) via savestate + F9 framebuffer
  snaps; this file is kept only as a record of the VDC/VCE decode + BAT-render path.


Mesen renders the PCE through a GPU surface a GDI/PrintWindow grab reads as black,
and its F12 capture is focus-flaky over RDP — so, exactly like the C64/Apple II/
VIC-20 ports run on a py65 6502 core and the GBA/WonderSwan ports on Unicorn, this
verifies the port headlessly by running the REAL HuCard ROM on a py65 65C02 core
extended with the few HuC6280 opcodes the kernel uses (TAM bank-mapping, CSH/CSL
speed) plus the HuC6280 MMU. It:

  * models the 8 MPR bank registers: a logical address -> MPR[addr>>13] selects a
    physical bank ($F8 = 8 KB internal RAM, $FF = I/O, else HuCard ROM = the file,
    bank B at file[B*0x2000]); at reset MPR7=0 so the reset vector $FFFE lands in
    bank 0 (the .cfg places the $E000 boot bank first in the file),
  * models the HuC6270 VDC write path (port $2000 selects a register; $2002/$2003
    write its data low/high) — MAWR sets the VRAM pointer, VWR writes a word and
    auto-increments — and the HuC6280 VCE colour table ($2402/$2404), and answers
    the VDC status read with the vblank bit set so `wait_vbl` advances,
  * then decodes the BAT (32x28 of 4bpp planar tiles from VRAM, each cell's CG
    pattern at (entry&0xFFF)<<4, colour through the 9-bit VCE palette GGGRRRBBB) to
    a 256x224 PNG.

The AUTOTEST ROMs drive the pad through the same input path; nothing is faked.

Usage: python pce/harness.py <rom.pce> <out.png> [instr_millions]
"""
import sys, struct, zlib
from py65.devices.mpu65c02 import MPU


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


class HuMem:
    """HuC6280 address space: MPR-mapped RAM / I/O / HuCard ROM + a VDC/VCE model."""
    def __init__(self, rom):
        self.rom = rom
        self.ram = bytearray(0x2000)        # 8 KB internal RAM (bank $F8)
        self.mpr = [0, 0, 0, 0, 0, 0, 0, 0]  # MPR7=0 at reset
        self.vram = [0] * 0x8000            # 32 K words
        self.vsel = 0                       # selected VDC register
        self.vlatch = 0
        self.vaddr = 0                      # MAWR/VWR pointer
        self.vce = [0] * 0x200
        self.vce_addr = 0
        self.vce_latch = 0

    def __getitem__(self, addr):
        bank = self.mpr[(addr >> 13) & 7]
        off = addr & 0x1FFF
        if bank == 0xF8:
            return self.ram[off]
        if bank == 0xFF:
            return self.io_read(addr & 0x1FFF)
        phys = bank * 0x2000 + off
        return self.rom[phys] if phys < len(self.rom) else 0xFF

    def __setitem__(self, addr, val):
        val &= 0xFF
        bank = self.mpr[(addr >> 13) & 7]
        off = addr & 0x1FFF
        if bank == 0xF8:
            self.ram[off] = val
        elif bank == 0xFF:
            self.io_write(addr & 0x1FFF, val)
        # ROM writes ignored

    def io_read(self, off):
        if off == 0x0000:
            return 0x20                     # VDC status: vblank bit set
        if off == 0x1000:
            return 0xFF                     # joypad idle (active-low; AUTOTEST bypasses this)
        return 0

    def io_write(self, off, val):
        if off == 0x0000:                   # VDC_AR
            self.vsel = val
        elif off == 0x0002:                 # VDC data low
            self.vlatch = val
        elif off == 0x0003:                 # VDC data high -> commit a word
            word = (val << 8) | self.vlatch
            if self.vsel == 0x00:           # MAWR
                self.vaddr = word
            elif self.vsel == 0x02:         # VWR
                self.vram[self.vaddr & 0x7FFF] = word
                self.vaddr = (self.vaddr + 1) & 0xFFFF
        elif off == 0x0402:                 # VCE_CTA low
            self.vce_addr = (self.vce_addr & 0x100) | val
        elif off == 0x0403:                 # VCE_CTA high
            self.vce_addr = (self.vce_addr & 0x0FF) | ((val & 1) << 8)
        elif off == 0x0404:                 # VCE_CTW low
            self.vce_latch = val
        elif off == 0x0405:                 # VCE_CTW high -> commit a colour
            self.vce[self.vce_addr & 0x1FF] = ((val & 1) << 8) | self.vce_latch
            self.vce_addr = (self.vce_addr + 1) & 0x1FF


def patch_huc6280(mpu):
    """Add the HuC6280 opcodes the kernel uses on top of py65's 65C02 core."""
    mpu.instruct = list(mpu.instruct)
    def op_tam(s):                          # TAM #imm : MPR[i]=A for each set bit
        mask = s.memory[s.pc]
        s.pc = (s.pc + 1) & 0xFFFF
        for i in range(8):
            if mask & (1 << i):
                s.memory.mpr[i] = s.a
        s.excycles += 3
    def op_nop1(s):                         # CSH/CSL : speed select (no timing model)
        pass
    mpu.instruct[0x53] = op_tam
    mpu.instruct[0xD4] = op_nop1            # CSH
    mpu.instruct[0x54] = op_nop1            # CSL


def vce_rgb(v):                       # 9-bit VCE colour, GGG RRR BBB (real HuC6260 order)
    b = (v & 7) * 255 // 7
    r = ((v >> 3) & 7) * 255 // 7
    g = ((v >> 6) & 7) * 255 // 7
    return r, g, b


def render(mem, out_path):
    W, H = 256, 224
    rgb = bytearray(W * H * 3)
    for ty in range(28):
        for tx in range(32):
            entry = mem.vram[ty * 32 + tx]
            cg = entry & 0x0FFF
            pal = (entry >> 12) & 0x0F
            base = (cg << 4) & 0x7FFF
            for r in range(8):
                w0 = mem.vram[(base + r) & 0x7FFF]
                w1 = mem.vram[(base + 8 + r) & 0x7FFF]
                py = ty * 8 + r
                for x in range(8):
                    bit = 7 - x
                    color = (((w0 >> bit) & 1) | (((w0 >> (8 + bit)) & 1) << 1) |
                             (((w1 >> bit) & 1) << 2) | (((w1 >> (8 + bit)) & 1) << 3))
                    idx = 0 if color == 0 else pal * 16 + color
                    rr, gg, bb = vce_rgb(mem.vce[idx])
                    o = ((py) * W + tx * 8 + x) * 3
                    rgb[o] = rr
                    rgb[o + 1] = gg
                    rgb[o + 2] = bb
    write_png(out_path, W, H, rgb)


def main():
    rom_path, out_path = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 3_000_000

    rom = open(rom_path, "rb").read()
    mem = HuMem(rom)
    mpu = MPU(memory=mem)
    patch_huc6280(mpu)
    mpu.pc = mem[0xFFFE] | (mem[0xFFFF] << 8)   # HuC6280 reset vector

    last_pc = -1
    stuck = 0
    for i in range(budget):
        mpu.step()
        # (no hard stop on a tight wait loop — wait_vbl exits via the status bit)
    render(mem, out_path)
    print("wrote %s (256x224) after %d steps; pc=%04X mpr=%s" %
          (out_path, budget, mpu.pc, [hex(b) for b in mem.mpr]))


if __name__ == "__main__":
    main()
