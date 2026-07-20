#!/usr/bin/env python3
"""ROM-free Game Boy Advance test harness for UnoDOS/GBA (Unicorn ARM core).

mGBA loads the ROM (its window title shows the internal title) but its deferred
OpenGL display will not render to a focus-independent grab under headless RDP. So
- exactly like the C64/Apple II ports use a py65 6502 core and the MacPlus port
uses a Unicorn 68K core - this verifies the port headlessly: it runs the real ARM
ROM on a Unicorn ARM7TDMI, services the two I/O registers the kernel polls
(VCOUNT so `wait_vblank` advances, KEYINPUT for the pad), and renders the Mode 3
framebuffer (VRAM 0x06000000, 240x160 BGR555) to a PNG. The AUTOTEST ROMs drive
the pad themselves, so the harness only has to tick the scanline counter.

Usage: python gba/harness.py <rom.gba> <out.png> [instr_millions] [--storage f.bin]

--storage persists the 4 KB USV1 region (EWRAM 0x02000000..0x02000FFF) across
runs: the sidecar file is loaded into EWRAM before the ROM boots and written
back after the run — the same storage-sidecar scheme the C64 harness uses.
"""
import os, sys, struct, zlib
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC

ROM   = 0x08000000
IWRAM = 0x03000000
EWRAM = 0x02000000
PAL   = 0x05000000
VRAM  = 0x06000000
OAM   = 0x07000000
IO    = 0x04000000
KEYINPUT = 0x04000130
VCOUNT   = 0x04000006

def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y*w*3:(y+1)*w*3]
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))

def main():
    args = sys.argv[1:]
    storage = None
    if "--storage" in args:
        i = args.index("--storage")
        storage = args[i + 1]
        del args[i:i + 2]
    rom_path, out_path = args[0], args[1]
    budget = int(float(args[2]) * 1_000_000) if len(args) > 2 else 30_000_000

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    # memory map (sizes rounded to Unicorn's page granularity)
    uc.mem_map(ROM,   max(0x40000, (len(data) + 0xFFF) & ~0xFFF), UC_PROT_ALL)
    uc.mem_map(IWRAM, 0x8000, UC_PROT_ALL)
    uc.mem_map(EWRAM, 0x40000, UC_PROT_ALL)
    uc.mem_map(PAL,   0x1000, UC_PROT_ALL)
    uc.mem_map(VRAM,  0x18000, UC_PROT_ALL)
    uc.mem_map(OAM,   0x1000, UC_PROT_ALL)
    uc.mem_map(IO,    0x1000, UC_PROT_ALL)
    uc.mem_write(ROM, data)
    uc.reg_write(UC_ARM_REG_SP, 0x03007F00)
    uc.mem_write(KEYINPUT, struct.pack("<H", 0x03FF))   # no keys held (active-low)
    if storage and os.path.exists(storage):
        sd = open(storage, "rb").read()[:0x1000]
        uc.mem_write(EWRAM, sd)
        print("loaded storage sidecar %s (%d bytes)" % (storage, len(sd)))

    # Run in chunks, alternating VCOUNT across the vblank threshold (160) so the
    # kernel's wait_vblank (poll <160 then >=160) makes one frame of progress per
    # pair of chunks. The AUTOTEST ROM advances its own pad script per frame.
    CHUNK = 20000
    pc = ROM
    vc = 0
    chunks = max(1, budget // CHUNK)
    for i in range(chunks):
        vc = 100 if (i & 1) else 200          # below / above the 160 vblank line
        uc.mem_write(VCOUNT, struct.pack("<H", vc))
        try:
            uc.emu_start(pc, ROM + 0x2000000, count=CHUNK)
        except Exception as e:
            print("  (stopped at chunk %d: %s)" % (i, e))
            break
        pc = uc.reg_read(UC_ARM_REG_PC)

    # render Mode 3 framebuffer (240x160, BGR555)
    fb = uc.mem_read(VRAM, 240 * 160 * 2)
    rgb = bytearray(240 * 160 * 3)
    for i in range(240 * 160):
        v = fb[i*2] | (fb[i*2+1] << 8)
        r = (v & 31) * 255 // 31
        g = ((v >> 5) & 31) * 255 // 31
        b = ((v >> 10) & 31) * 255 // 31
        rgb[i*3] = r
        rgb[i*3+1] = g
        rgb[i*3+2] = b
    write_png(out_path, 240, 160, rgb)
    if storage:
        with open(storage, "wb") as f:
            f.write(uc.mem_read(EWRAM, 0x1000))
        print("wrote storage sidecar %s (4096 bytes)" % storage)
    print("wrote %s (240x160) after ~%d instrs" % (out_path, chunks * CHUNK))

if __name__ == "__main__":
    main()
