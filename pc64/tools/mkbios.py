#!/usr/bin/env python3
"""Build a legacy-BIOS bootable disk image for pc64.

    python3 tools/mkbios.py <boot.bin> <stage2.bin> <kernel.bin> <out.img>

The result boots BOTH WAYS from one file - see PART_TYPE_ESP below.

Layout (512-byte sectors), matching boot/bios_boot.asm and boot/bios_stage2.asm:

    0        boot sector + MBR partition table
    1..16    stage2
    17..     the kernel image, loaded verbatim to physical 0x100000
    16384..  one FAT32 partition, type 0xEF: the ESP to UEFI firmware, the OS
             volume to the running system, and invisible to the BIOS path

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
import os
import struct
import subprocess
import sys

SECTOR = 512
STAGE2_SECTORS = 16
KERNEL_LBA = 17
PATCH_MAGIC = b"BOBP"
MIN_IMAGE = 8 * 1024 * 1024      # see the padding note in main()

# The boot chain is read by LBA, not through a filesystem, so it lives in a
# reserved area BEFORE the partition. 8 MiB is four times the largest kernel
# built to date, which buys room to grow without ever moving the partition -
# and moving it would invalidate every image already written to a stick.
RESERVED_SECTORS = 16384         # 8 MiB

# 0xEF = EFI System Partition, and this ONE BYTE is what makes the image boot
# both ways. UEFI firmware scanning an MBR-partitioned disk looks for type 0xEF
# and runs \EFI\BOOT\BOOTX64.EFI from it; a BIOS ignores the type entirely and
# runs the code in the boot sector, which loads the kernel from the reserved
# area by raw LBA. So the same FAT volume is the ESP to one firmware and just
# the OS volume to the other, with no duplicated tree and nothing to keep in
# sync. pc64's own FAT layer already accepts 0xEF (fat.c), so the running system
# mounts it either way.
#
# MBR rather than a hybrid GPT deliberately. A hybrid GPT - a protective MBR
# rewritten to hold real entries - is what Boot Camp and rEFInd do, and it is a
# spec violation that some firmware rejects and some partitioning tools
# helpfully "repair". An MBR disk with an 0xEF partition is inside the UEFI
# spec, which permits MBR-partitioned media, and is what isohybrid images have
# used for years.
PART_TYPE_ESP = 0xEF


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


def mbr_entry(first_lba: int, sectors: int) -> bytes:
    """One MBR partition entry for the OS volume.

    The CHS fields are the 0xFE/0xFF/0xFF "beyond CHS, use LBA" sentinel rather
    than a computed geometry. Every machine this port targets reads the LBA
    fields; a computed CHS triple would be a second source of truth that is
    wrong for any disk over 8 GiB and right nowhere it matters.
    """
    return (bytes([0x80])                    # bootable
            + bytes([0xFE, 0xFF, 0xFF])      # start CHS: use LBA
            + bytes([PART_TYPE_ESP])
            + bytes([0xFE, 0xFF, 0xFF])      # end CHS: use LBA
            + struct.pack("<II", first_lba, sectors))


def build_fat(esp_dir: str, sectors: int, out: str) -> None:
    """Format a FAT32 volume of `sectors` and copy the whole build/esp tree in.

    Same mtools path tools/mkuefi.py uses for the UEFI image, for the same
    reason: the kernel's own FAT writer cannot be used to author the volume it
    is going to be loaded from.
    """
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(["mformat", "-C", "-i", out, "-T", str(sectors),
                    "-h", "64", "-s", "32", "-F", "-v", "UNODOS", "::"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name in sorted(os.listdir(esp_dir)):
        src = os.path.join(esp_dir, name)
        cmd = (["mcopy", "-s", "-i", out, src, "::/"] if os.path.isdir(src)
               else ["mcopy", "-i", out, src, "::/" + name])
        subprocess.run(cmd, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def patch(stage2: bytearray, kern_sectors: int, entry: int) -> None:
    at = stage2.find(PATCH_MAGIC)
    if at < 0:
        raise SystemExit("mkbios: patch magic 'BOBP' not found in stage2")
    struct.pack_into("<I", stage2, at + 4, kern_sectors)
    struct.pack_into("<I", stage2, at + 8, entry)


def main() -> int:
    if len(sys.argv) not in (5, 6):
        print(__doc__)
        return 2
    bootf, stage2f, kernelf, outf = sys.argv[1:5]
    esp_dir = sys.argv[5] if len(sys.argv) == 6 else None

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

    # STAGE THE PATCHED CHAIN ON THE VOLUME, for the on-device installer.
    #
    # `Install` cannot author a BIOS-bootable disk out of nothing: it needs the
    # boot sector, stage2 and the kernel as bytes. Shipping the ALREADY-PATCHED
    # stage2 is what keeps that simple - the patch (sector count + PE entry
    # address) is computed here, from the very kernel being shipped beside it,
    # so the installer copies three blobs to three LBAs and has no parsing to
    # get wrong. It also means a UEFI-booted machine can install a
    # BIOS-bootable disk, since the chain travels with the tree rather than
    # having to be recovered from the disk we happen to have booted.
    if esp_dir and os.path.isdir(esp_dir):
        bootdir = os.path.join(esp_dir, "BOOT")
        os.makedirs(bootdir, exist_ok=True)
        open(os.path.join(bootdir, "BOOT.BIN"), "wb").write(bytes(boot))
        open(os.path.join(bootdir, "STAGE2.BIN"), "wb").write(bytes(stage2))
        open(os.path.join(bootdir, "UNODOS.SYS"), "wb").write(kernel)

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

    # ---- the OS volume ----------------------------------------------------
    # WITHOUT THIS THE IMAGE BOOTS AND HAS NO FILESYSTEM, which looks like a
    # working system right up until an app is launched: the desktop is drawn
    # from the kernel image, but every .UNO module, font and media file lives
    # on disk. It sits after RESERVED_SECTORS so the boot chain, which stage2
    # reads by raw LBA, is never inside a partition anything could reformat.
    fat_note = "no OS volume"
    if esp_dir and os.path.isdir(esp_dir):
        if KERNEL_LBA + kern_sectors > RESERVED_SECTORS:
            raise SystemExit(
                "mkbios: the kernel ends at LBA %d, past the %d-sector reserved "
                "area - raise RESERVED_SECTORS (and reflash every existing "
                "image, since the partition would move)"
                % (KERNEL_LBA + kern_sectors, RESERVED_SECTORS))
        # Size the disk to the tree it has to carry: the tree plus half again
        # plus 16 MiB of slack for what the running system writes (telemetry,
        # documents), never below 96 MiB.
        tree = 0
        for root, _dirs, files in os.walk(esp_dir):
            for fn in files:
                tree += os.path.getsize(os.path.join(root, fn))
        need = RESERVED_SECTORS * SECTOR + tree + tree // 2 + 16 * 1024 * 1024
        need = max(need, 96 * 1024 * 1024)
        need = (need + 1024 * 1024 - 1) // (1024 * 1024) * (1024 * 1024)
        if len(img) < need:
            img += b"\0" * (need - len(img))

        part_sectors = len(img) // SECTOR - RESERVED_SECTORS
        fat = "/tmp/uno_bios_fat.img"
        build_fat(esp_dir, part_sectors, fat)
        with open(fat, "rb") as f:
            data = f.read()
        img[RESERVED_SECTORS * SECTOR:RESERVED_SECTORS * SECTOR + len(data)] = data
        os.remove(fat)
        img[0x1BE:0x1CE] = mbr_entry(RESERVED_SECTORS, part_sectors)
        fat_note = "ESP/FAT32 at LBA %d (%d MiB), boots BIOS + UEFI" % (
            RESERVED_SECTORS, part_sectors // 2048)

    with open(outf, "wb") as f:
        f.write(img)

    print("mkbios: %s  kernel %d bytes (%d sectors) entry %#x  image %d MiB  %s"
          % (outf, len(kernel), kern_sectors, entry, len(img) // (1024 * 1024),
             fat_note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
