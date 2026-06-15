#!/usr/bin/env python3
"""Pack a bootable UnoDOS/IIGS 800 KB ProDOS-order disk image.

Layout (512-byte blocks):
  block 0      = boot.bin (the block-0 boot stage; byte 0 = $01 signature)
  block 1..K   = kernel.bin (loaded to $00:2000 by the boot stage)
  block K+1..  = reserved (FAT12 volume lands here at M2; see FS_START_BLOCK)

The boot stage stops its read loop when the running block number reaches
KBLOCKS+1, encoded as the unique immediate of "CMP #$4B" (C9 4B) in boot.s;
we patch that byte to KBLOCKS+1 here (the apple2/mkdsk.py ktpatch pattern).

Usage: mkdsk.py <boot.bin> <kernel.bin> <out.po>
"""
import sys

BLOCK = 512
BLOCKS = 1600                       # 800 KB 3.5" disk
DISK = BLOCK * BLOCKS               # 819200

KERNLOAD = 0x2000                   # boot loads the kernel here (bank 0)
KERN_TOP = 0xC000                   # bank-0 RAM ceiling (I/O begins at $C000)
FS_START_BLOCK = 256                # M2 FAT12 volume origin (128 KB reserved)

boot_p, kern_p, out_p = sys.argv[1:4]
boot = bytearray(open(boot_p, "rb").read())
kern = open(kern_p, "rb").read()

assert len(boot) == BLOCK, f"boot.bin must be {BLOCK} bytes, got {len(boot)}"
assert boot[0] == 0x01, f"boot.bin byte 0 must be $01 (ProDOS sig), got {boot[0]:#x}"
assert KERNLOAD + len(kern) <= KERN_TOP, \
    f"kernel ({len(kern)}B @ {KERNLOAD:#x}) overruns bank-0 RAM at {KERN_TOP:#x}"

kblocks = (len(kern) + BLOCK - 1) // BLOCK
assert 1 + kblocks <= FS_START_BLOCK, \
    f"kernel needs {kblocks} blocks; would overlap the FS region at block {FS_START_BLOCK}"
patch = kblocks + 1
assert 0 < patch <= 0xFF, f"KBLOCKS+1 ({patch}) doesn't fit a byte (boot loop is 8-bit)"

# patch the unique "CMP #$4B" (C9 4B) -> "CMP #(KBLOCKS+1)"
at = boot.find(b"\xC9\x4B")
assert at >= 0, "ktpatch 'CMP #$4B' (C9 4B) not found in boot.bin"
assert boot.find(b"\xC9\x4B", at + 1) < 0, "more than one CMP #$4B candidate?"
boot[at + 1] = patch

img = bytearray(DISK)
img[0:BLOCK] = boot
img[BLOCK:BLOCK + len(kern)] = kern
open(out_p, "wb").write(img)
print(f"{out_p}: kernel {len(kern)} bytes ({kblocks} block(s)), "
      f"KBLOCKS+1={patch}, image {DISK} bytes ({BLOCKS} blocks / 800 KB)")
