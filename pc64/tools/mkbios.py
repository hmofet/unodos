#!/usr/bin/env python3
"""Build a legacy-BIOS bootable disk image for pc64.

    python3 tools/mkbios.py <boot.bin> <stage2.bin> <kernel.bin> <out.img>

Layout (512-byte sectors), matching boot/bios_boot.asm and boot/bios_stage2.asm:

    0        boot sector
    1..16    stage2
    17..     the kernel image, loaded verbatim to physical 0x100000

TWO VALUES ARE PATCHED INTO STAGE2, and both have to be, because a 16-bit
loader cannot work either of them out for itself:

  kern_count    how many sectors to read. stage2 has no filesystem - it reads a
                run of sectors at a fixed LBA. That is deliberate: a FAT walk in
                the boot path is a second implementation of a thing the kernel
                already does properly, and it would have to be right before any
                of the kernel's own code has run.

  kernel_entry  the absolute address to jump to. The kernel is a PE image
                linked at 0x100000, so its first bytes are headers, not code.
                Jumping to 0x100000 would execute 'MZ' as instructions. The
                entry point comes from the PE optional header's
                AddressOfEntryPoint, read here rather than hardcoded, because it
                moves whenever the link order does.

Both are written over placeholder dwords located by scanning for the magic
'BOBP', so neither the assembler nor this script needs to know the other's
layout.
"""
import struct
import sys

SECTOR = 512
STAGE2_SECTORS = 16
KERNEL_LBA = 17
PATCH_MAGIC = b"BOBP"
MIN_IMAGE = 8 * 1024 * 1024      # see the padding note in main()


def pe_entry_va(image: bytes) -> int:
    """Absolute entry address of a PE32+ image = ImageBase + AddressOfEntryPoint."""
    if image[:2] != b"MZ":
        raise SystemExit("mkbios: kernel is not a PE image (no MZ) - "
                         "expected the flat-linked BIOS build")
    pe_off = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit("mkbios: bad PE signature")
    opt = pe_off + 24
    magic = struct.unpack_from("<H", image, opt)[0]
    if magic != 0x20B:
        raise SystemExit("mkbios: kernel is not PE32+ (magic %#x) - "
                         "the BIOS kernel must be 64-bit" % magic)
    entry_rva = struct.unpack_from("<I", image, opt + 16)[0]
    image_base = struct.unpack_from("<Q", image, opt + 24)[0]
    return image_base + entry_rva, image_base


def patch(stage2: bytearray, kern_sectors: int, entry: int) -> None:
    at = stage2.find(PATCH_MAGIC)
    if at < 0:
        raise SystemExit("mkbios: patch magic 'BOBP' not found in stage2")
    struct.pack_into("<I", stage2, at + 4, kern_sectors)
    struct.pack_into("<I", stage2, at + 8, entry)


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    bootf, stage2f, kernelf, outf = sys.argv[1:]

    boot = bytearray(open(bootf, "rb").read())
    stage2 = bytearray(open(stage2f, "rb").read())
    kernel = open(kernelf, "rb").read()

    if len(boot) != SECTOR:
        raise SystemExit("mkbios: boot sector is %d bytes, must be exactly %d"
                         % (len(boot), SECTOR))
    if boot[510:512] != b"\x55\xaa":
        raise SystemExit("mkbios: boot sector lacks the 0xAA55 signature")
    if len(stage2) > STAGE2_SECTORS * SECTOR:
        raise SystemExit("mkbios: stage2 is %d bytes, over the %d-byte window "
                         "the boot sector reads (raise STAGE2_SECTORS in BOTH "
                         "bios_boot.asm and this script)"
                         % (len(stage2), STAGE2_SECTORS * SECTOR))

    entry, base = pe_entry_va(kernel)
    if base != 0x100000:
        raise SystemExit("mkbios: kernel image base is %#x, expected 0x100000 - "
                         "stage2 loads it there and does not relocate" % base)

    kern_sectors = (len(kernel) + SECTOR - 1) // SECTOR
    patch(stage2, kern_sectors, entry)

    img = bytearray()
    img += boot
    img += stage2.ljust(STAGE2_SECTORS * SECTOR, b"\0")
    assert len(img) == KERNEL_LBA * SECTOR
    img += kernel
    if len(img) % SECTOR:
        img += b"\0" * (SECTOR - len(img) % SECTOR)

    # PAD TO A PLAUSIBLE DISK. A raw image only as long as its contents is
    # smaller than a single cylinder, and SeaBIOS declines to treat it as a hard
    # disk at all - the symptom is "Boot failed: could not read the boot disk"
    # with a boot sector that is perfectly valid. Nothing real is ever that
    # small, so this is fidelity rather than a workaround.
    if len(img) < MIN_IMAGE:
        img += b"\0" * (MIN_IMAGE - len(img))
    if len(img) % (1024 * 1024):
        img += b"\0" * (1024 * 1024 - len(img) % (1024 * 1024))

    with open(outf, "wb") as f:
        f.write(img)

    print("mkbios: %s  kernel %d bytes (%d sectors) entry %#x  image %d KB"
          % (outf, len(kernel), kern_sectors, entry, len(img) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
