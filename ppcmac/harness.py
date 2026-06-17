#!/usr/bin/env python3
"""ROM-free PowerPC Mac test harness for UnoDOS/ppcmac (Unicorn PPC32, big-endian).

The PowerPC port boots over Open Firmware (no Mac OS): OF enters the client program
with the IEEE-1275 client-interface entry in r5, and the kernel makes a few OF calls
to find the `screen` device and read its framebuffer address + linebytes, then draws
into that surface. A real Mac renders to a display no headless grab can read, so —
like every other fresh port — this verifies headlessly on a from-scratch core:

  * run the real PowerPC payload on a Unicorn PPC32 big-endian core,
  * EMULATE the Open Firmware client interface: r5 points at a one-instruction `blr`
    trampoline; a code hook there reads the CI argument array (r3), services
    `finddevice` / `getprop` (handing back a fixed framebuffer base + pitch), and
    returns — so fb_init gets a real surface to draw into,
  * run an instruction budget (wait_vblank is a spin loop, so frames advance and the
    AUTOTEST pad plays out), then render the framebuffer to a PNG.

Usage: python ppcmac/harness.py <unodos.bin> <out.png> [instr_millions]
"""
import sys, struct, zlib
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_PROT_ALL, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_5, UC_PPC_REG_PC

W, H = 640, 480
PITCH = W * 4
LOAD     = 0x00100000
RAM_BASE = 0x00000000
RAM_SIZE = 0x02000000           # 32 MB covers low mem + kernel + stack + vars + FB
FB_PA    = 0x01000000
OF_ENTRY = 0x00000100           # the OF client-interface trampoline (a single blr)


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


def be32(uc, addr):
    return struct.unpack(">I", uc.mem_read(addr, 4))[0]


def cstr(uc, addr):
    out = bytearray()
    while True:
        b = uc.mem_read(addr, 1)[0]
        if b == 0:
            break
        out.append(b)
        addr += 1
    return out.decode("latin-1")


def of_service(uc):
    """Service one Open Firmware client-interface call (r3 = &CI array)."""
    arr = uc.reg_read(UC_PPC_REG_3)
    nargs = be32(uc, arr + 4)
    name = cstr(uc, be32(uc, arr + 0))
    rets_off = 12 + 4 * nargs

    def wr(off, v):
        uc.mem_write(arr + off, struct.pack(">I", v & 0xFFFFFFFF))

    if name in ("finddevice", "instance-to-package", "open"):
        wr(rets_off, 0x11111111)              # a device/instance handle token
    elif name == "getprop":
        prop = cstr(uc, be32(uc, arr + 16))
        buf = be32(uc, arr + 20)
        val = {"address": FB_PA, "frame-buffer-adr": FB_PA, "linebytes": PITCH,
               "width": W, "height": H, "depth": 32, "stdout": 0x22222222}.get(prop, 0)
        uc.mem_write(buf, struct.pack(">I", val))
        wr(rets_off, 4)                       # property length returned
    uc.reg_write(UC_PPC_REG_3, 0)             # catch-result: success


def main():
    rom_path, out_path = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 120_000_000

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mem_write(LOAD, data)
    uc.mem_write(OF_ENTRY, struct.pack(">I", 0x4E800020))   # blr
    uc.reg_write(UC_PPC_REG_5, OF_ENTRY)                    # OF client entry in r5

    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, ud: of_service(uc),
                None, OF_ENTRY, OF_ENTRY)

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_PPC_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc = LOAD
    ran = 0
    while ran < budget:
        try:
            uc.emu_start(pc, LOAD + 0x100000, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_PPC_REG_PC)
        ran += CHUNK

    fb = uc.mem_read(FB_PA, W * H * 4)
    rgb = bytearray(W * H * 3)
    for i in range(W * H):
        rgb[i*3]   = fb[i*4+1]   # R
        rgb[i*3+1] = fb[i*4+2]   # G
        rgb[i*3+2] = fb[i*4+3]   # B
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs" % (out_path, W, H, ran // 1_000_000))


if __name__ == "__main__":
    main()
