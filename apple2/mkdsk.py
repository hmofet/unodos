#!/usr/bin/env python3
"""Pack a bootable UnoDOS/AppleII 140K Disk II image (DOS 3.3 logical order).

Layout: track 0 = boot.bin (exactly 4096 bytes, byte 0 = $10 autoload
count, contains the GCR read-RWTS). Kernel image from track 1 (byte 4096),
zero-padded to a whole number of tracks. The 'ktpatch' placeholder
(CMP #$4B at boot.s) is patched with KTRACKS+1 so boot0's track loop
stops after loading exactly the kernel's tracks.

Usage: mkdsk.py <boot.bin> <kernel.bin> <out.dsk>
"""
import sys, struct

TRACK = 4096            # 16 sectors * 256 bytes
TRACKS = 35
DISK = TRACK * TRACKS    # 143360 bytes, 140K

# kernel buffers (BSS) start here - the kernel image must stay below it
KBSS = 0x6000
KERNLOAD = 0x4000

boot_p, kern_p, out_p = sys.argv[1:4]
boot = bytearray(open(boot_p, "rb").read())
kern = open(kern_p, "rb").read()

assert len(boot) == TRACK, f"boot.bin must be {TRACK} bytes, got {len(boot)}"
assert boot[0] == 0x10, f"boot.bin byte 0 must be $10 (16-sector autoload), got {boot[0]:#x}"

assert KERNLOAD + len(kern) <= KBSS, \
    f"kernel ({len(kern)}B @ {KERNLOAD:#x}) would overlap KBSS at {KBSS:#x}"

ktracks = (len(kern) + TRACK - 1) // TRACK
assert 1 + ktracks <= TRACKS, f"kernel needs {ktracks} tracks, only {TRACKS-1} available"

# patch the unique "CMP #$4B" (C9 4B) -> "CMP #(KTRACKS+1)"
at = boot.find(b"\xC9\x4B")
assert at >= 0, "ktpatch 'CMP #$4B' (C9 4B) not found in boot.bin"
assert boot.find(b"\xC9\x4B", at + 1) < 0, "more than one CMP #$4B candidate?"
patch = ktracks + 1
assert 0 <= patch <= 0xFF, f"KTRACKS+1 ({patch}) doesn't fit a byte"
boot[at + 1] = patch

img = bytearray(DISK)
img[0:TRACK] = boot
img[TRACK:TRACK + len(kern)] = kern
open(out_p, "wb").write(img)
print(f"{out_p}: kernel {len(kern)} bytes ({ktracks} track(s)), "
      f"KTRACKS+1={patch}, image {DISK} bytes ({TRACKS} tracks)")
