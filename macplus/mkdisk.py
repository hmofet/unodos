#!/usr/bin/env python3
"""Pack a bootable UnoDOS/MacPlus 800K floppy image.

Layout: sectors 0-1 = boot blocks (boot.bin, exactly 1024 bytes), kernel
image from byte 1024 (where the boot code PBReads it from), zero fill to
800K. The 'KSIZ' placeholder in the boot block's ioReqCount is patched
with the sector-rounded kernel size.

Usage: mkdisk.py <boot.bin> <kernel.bin> <out.dsk>
"""
import sys, struct

SECTOR = 512
DISK = 819200  # 800K GCR double-sided

boot_p, kern_p, out_p = sys.argv[1:4]
boot = bytearray(open(boot_p, "rb").read())
kern = open(kern_p, "rb").read()

assert len(boot) == 1024, f"boot blocks must be 1024 bytes, got {len(boot)}"
assert boot[0:2] == b"\x4c\x4b", "boot blocks missing 'LK' signature"

ksize = (len(kern) + SECTOR - 1) // SECTOR * SECTOR
assert 1024 + ksize <= DISK, "kernel too large for an 800K disk"
# the kernel loads at $20000 and must stay below KBSS ($30000), where the
# out-of-image buffers live (sync: kernel.asm KBSS)
assert len(kern) <= 0x10000, f"kernel ({len(kern)}B) would overlap KBSS at +64KB"

at = boot.find(b"KSIZ")
assert at > 0, "ioReqCount placeholder 'KSIZ' not found in boot blocks"
boot[at:at+4] = struct.pack(">I", ksize)
assert boot.find(b"KSIZ") < 0, "more than one KSIZ placeholder?"

img = bytearray(DISK)
img[0:1024] = boot
img[1024:1024+len(kern)] = kern
open(out_p, "wb").write(img)
print(f"{out_p}: kernel {len(kern)} bytes ({ksize} on disk), image {DISK}")
