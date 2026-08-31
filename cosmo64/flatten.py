#!/usr/bin/env python3
"""flatten.py -- PE32+ (aarch64) -> flat LK payload for pc64-on-ARM.

The kernel image is linked by llvm-mingw's lld at a FIXED image base equal to
LK's load address (0x40080000), so no relocation is needed at all: this just
lays the sections out at their RVAs -- the same flattening mkuno.py does for
.UNO app modules, minus the reloc/import tables -- and, because nothing loads
PE headers at run time, uses image offset 0 (where the headers would sit) for
a single AArch64 `b` to the PE entry point. LK branches to the first byte of
the flat image; that byte branches to _start.

The .bss stays as zeros inside the image (no trim): the payload then needs no
runtime bss clear, and cosmo/mkbootimg.py gzips it, so the zeros cost nothing.

Usage: flatten.py <in.exe> <out.bin>
"""
import struct, sys

LOAD = 0x40080000


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = open(sys.argv[1], "rb").read()
    if d[:2] != b"MZ":
        sys.exit("flatten: not a PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        sys.exit("flatten: bad PE signature")
    machine, nsect, _, _, _, opt_sz, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
    if machine != 0xAA64:
        sys.exit("flatten: not AArch64 (machine 0x%04x)" % machine)
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        sys.exit("flatten: not PE32+")
    entry = struct.unpack_from("<I", d, opt + 16)[0]
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    size_image = struct.unpack_from("<I", d, opt + 56)[0]
    nddir = struct.unpack_from("<I", d, opt + 108)[0]
    if image_base != LOAD:
        sys.exit("flatten: image base 0x%X, must be 0x%X (link with "
                 "-Wl,--image-base,0x%X)" % (image_base, LOAD, LOAD))

    def ddir(i):
        return struct.unpack_from("<II", d, opt + 112 + 8 * i) if i < nddir else (0, 0)

    img = bytearray(size_image)
    for i in range(nsect):
        _, vsz, va, rsz, roff = struct.unpack_from("<8sIIII", d, opt + opt_sz + 40 * i)
        n = min(vsz, rsz)
        if n:
            img[va:va + n] = d[roff:roff + n]

    # real PE imports mean a symbol resolved against a DLL nothing will load
    irva, isz = ddir(1)
    p = irva
    while p and p + 20 <= irva + isz:
        if any(struct.unpack_from("<5I", img, p)):
            sys.exit("flatten: image has real PE imports -- freestanding only")
        p += 20

    # the base matches, so relocations are moot; drop their bytes
    rrva, rsz = ddir(5)
    if rsz:
        img[rrva:rrva + rsz] = b"\0" * rsz

    if entry < 4 or entry >= size_image or entry & 3:
        sys.exit("flatten: entry RVA 0x%X out of range" % entry)
    if LOAD + size_image > 0x402E0000:
        sys.exit("flatten: image ends at 0x%X -- collides with the stack/VARS "
                 "region at 0x402E0000+" % (LOAD + size_image))
    # The headers' spot doubles as an ARM64 Image header (the pinephone port's
    # p-boot trick): LK just executes offset 0, where code0 = `b <entry>`, while
    # QEMU's -kernel loader reads text_offset 0x80000 and lands the image at
    # RAM base + 0x80000 = exactly the link address, DTB pointer in x0 -- the
    # same contract LK provides. One binary, both loaders.
    struct.pack_into("<I", img, 0, 0x14000000 | (entry >> 2))   # code0: b entry
    struct.pack_into("<I", img, 4, 0xD503201F)                  # code1: nop
    struct.pack_into("<Q", img, 8, 0x80000)                     # text_offset
    struct.pack_into("<Q", img, 16, size_image)                 # image_size
    struct.pack_into("<Q", img, 24, 0)                          # flags: LE, 4K
    struct.pack_into("<I", img, 56, 0x644D5241)                 # magic "ARM\x64"

    open(sys.argv[2], "wb").write(img)
    print("flatten: %s (%d bytes, entry RVA 0x%X, base 0x%X)"
          % (sys.argv[2], len(img), entry, image_base))


if __name__ == "__main__":
    main()
