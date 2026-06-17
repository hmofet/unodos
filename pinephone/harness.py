#!/usr/bin/env python3
"""ROM-free PinePhone (Allwinner A64, AArch64) test harness for UnoDOS/pinephone.

Like the Pi port runs its kernel on a Unicorn Cortex-A, this verifies the PinePhone
port headlessly. The A64 is simpler to model than the Pi here: there is no GPU
mailbox and no peripheral-timer poll — the kernel programs the DE2 mixer UI layer
to scan out a fixed DRAM framebuffer (PINE_FB), and paces frames off the ARM
architectural generic timer (cntpct_el0), which Unicorn advances on its own. So the
harness just:

  * maps DRAM (kernel + stack + vars + the framebuffer) and a harmless RAM sink over
    the DE2 register block (the layer pokes land here),
  * runs the real payload for an instruction budget (cntpct_el0 advances, so
    wait_vblank returns one frame per loop and the AUTOTEST pad plays out),
  * renders the DE2 framebuffer at PINE_FB to a PNG.

Usage: python pinephone/harness.py <unodos.bin> <out.png> [instr_millions]
"""
import sys, struct, zlib
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC

W, H = 480, 640
LOAD     = 0x40080000
DRAM     = 0x40000000
DRAM_SZ  = 0x01000000           # 16 MB covers kernel + stack + vars + framebuffer
PINE_FB  = 0x40400000
DE2_BASE = 0x01000000           # display engine register block (sunk to RAM)
DE2_SZ   = 0x00200000


def write_png(path, w, h, rgb):
    raw = bytearray()
    row = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y*row:(y+1)*row]
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    rom_path, out_path = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 160_000_000

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(DRAM, DRAM_SZ, UC_PROT_ALL)
    uc.mem_map(DE2_BASE, DE2_SZ, UC_PROT_ALL)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x40200000)

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc = LOAD
    ran = 0
    while ran < budget:
        try:
            uc.emu_start(pc, DRAM + DRAM_SZ, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_ARM64_REG_PC)
        ran += CHUNK

    fb = uc.mem_read(PINE_FB, W * H * 4)
    rgb = bytearray(W * H * 3)
    for i in range(W * H):
        w = fb[i*4] | (fb[i*4+1] << 8) | (fb[i*4+2] << 16)
        rgb[i*3]   = (w >> 16) & 0xFF
        rgb[i*3+1] = (w >> 8) & 0xFF
        rgb[i*3+2] = w & 0xFF
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs" % (out_path, W, H, ran // 1_000_000))


if __name__ == "__main__":
    main()
