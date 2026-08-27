#!/usr/bin/env python3
"""ROM-free Cosmo Communicator (MediaTek MT6771, AArch64) test harness for UnoDOS/cosmo.

The Cosmo port adopts Planet LK's live framebuffer instead of bringing up a display,
so this harness is the simplest of the AArch64 ports: it plays LK. It

  * maps DRAM (payload + stack + vars + a stand-in panel framebuffer) and a sink page
    over the TOPRGU watchdog (the payload's first act is to disable it),
  * pre-seeds FBINFO with a panel-native framebuffer base + pitch, exactly as LK hands
    it to us via the DTB videolfb properties on hardware,
  * runs the real payload for an instruction budget (cntpct_el0 advances on its own in
    Unicorn, so wait_vblank returns one frame per loop and the AUTOTEST pad plays out),
  * reads back the centred draw origin fb_init computed and renders the 640x480 UI to
    a PNG (the UI is centred in LK's 2160x1080 landscape panel).

Usage: python cosmo/harness.py <unodos.bin> <out.png> [instr_millions]
"""
import sys, struct, zlib, os
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_X0

W, H = 640, 480                       # SCRW x SCRH (the UI surface)
PANEL_W, PANEL_H = 2160, 1080         # LK's landscape panel
LOAD    = 0x40080000
DRAM    = 0x40000000
DRAM_SZ = 0x01000000                  # 16 MB: payload + stack + vars + panel FB
FBINFO  = 0x40320000                  # fb_base(8) + fb_pitch(4), pre-seeded like LK
PANEL_FB    = 0x40400000              # stand-in for LK's adopted framebuffer base
PANEL_PITCH = PANEL_W * 4             # 8640 bytes/row
WDT_PAGE = 0x10007000                 # TOPRGU watchdog (payload writes the disable key)


def sink_read(uc, offset, size, ud):
    return 0


def sink_write(uc, offset, size, value, ud):
    pass


def write_png(path, w, h, rgb):
    raw = bytearray()
    row = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * row:(y + 1) * row]

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    argv = [a for a in sys.argv if not a.startswith("--")]
    rom_path, out_path = argv[1], argv[2]
    budget = int(float(argv[3]) * 1_000_000) if len(argv) > 3 else 60_000_000

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(DRAM, DRAM_SZ, UC_PROT_ALL)
    uc.mmio_map(WDT_PAGE & ~0xFFF, 0x1000, sink_read, None, sink_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x40200000)
    uc.reg_write(UC_ARM64_REG_X0, DRAM)          # DTB pointer (unused by the scaffold)
    # Play LK: hand the payload a live panel framebuffer via FBINFO.
    uc.mem_write(FBINFO, struct.pack("<QI", PANEL_FB, PANEL_PITCH))

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc, ran = LOAD, 0
    while ran < budget:
        try:
            uc.emu_start(pc, DRAM + DRAM_SZ, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_ARM64_REG_PC)
        ran += CHUNK

    # fb_init wrote the centred draw origin + panel stride back into FBINFO.
    fb_base, fb_pitch = struct.unpack("<QI", bytes(uc.mem_read(FBINFO, 12)))
    rgb = bytearray(W * H * 3)
    for y in range(H):
        rowaddr = fb_base + y * fb_pitch
        rowpx = uc.mem_read(rowaddr, W * 4)
        for x in range(W):
            w = rowpx[x*4] | (rowpx[x*4+1] << 8) | (rowpx[x*4+2] << 16)
            o = (y * W + x) * 3
            rgb[o]   = (w >> 16) & 0xFF
            rgb[o+1] = (w >> 8) & 0xFF
            rgb[o+2] = w & 0xFF
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs; draw origin 0x%X pitch %d"
          % (out_path, W, H, ran // 1_000_000, fb_base, fb_pitch))


if __name__ == "__main__":
    main()
