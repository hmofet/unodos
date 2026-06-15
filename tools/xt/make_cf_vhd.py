#!/usr/bin/env python3
# make_cf_vhd.py - Build a bootable FAT12 "superfloppy" CompactFlash VHD for the
# UnoDOS 8088 port on an XT-IDE adapter.
#
# UnoDOS's hard-disk path (MBR/VBR/stage2_hd + the kernel FAT16 driver) is
# 386-only by design, so an 8088 cannot use a normal FAT16 CF. Instead we put
# the *exact* 1.44MB floppy layout (boot sector + stage2 + kernel reserved
# sectors + FAT12 filesystem) at the front of the CF. XT-IDE boots LBA 0 (our
# 8086-clean boot sector); stage2 + the kernel read with the CF's own CHS
# geometry (probed via INT 13h/08h), and the FAT12 driver mounts it. The CF is
# larger than 1.44MB; only the first 2880 sectors are used (deviation documented
# in docs/PORT-8088.md). FAT16-on-8088 for full-size CF cards is a follow-up.
#
# We start from MartyPC's default_xtide.vhd so the output keeps a valid VHD
# footer + a sane XT-IDE CHS geometry (cyl=615, heads=4, spt=26), then overlay
# the floppy image onto the data area (offset 0), leaving the trailing footer
# intact.
import sys, os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
XT   = r"C:\Users\arin\xt-tools"

floppy   = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "unodos-144.img")
template = sys.argv[2] if len(sys.argv) > 2 else os.path.join(XT, "media", "hdds", "default_xtide.vhd")
out      = sys.argv[3] if len(sys.argv) > 3 else os.path.join(XT, "media", "hdds", "unodos-cf.vhd")

with open(floppy, "rb") as f:
    img = f.read()

if not os.path.exists(template):
    sys.exit(f"template VHD not found: {template}")

shutil.copyfile(template, out)
size = os.path.getsize(out)
if len(img) > size - 512:
    sys.exit(f"floppy image ({len(img)}) too big for VHD data area ({size-512})")

# Overlay the floppy image at offset 0 (LBA 0); leave the rest + the 512-byte
# VHD footer untouched so the geometry is preserved.
with open(out, "r+b") as f:
    f.seek(0)
    f.write(img)

print(f"wrote {out} ({size} bytes); overlaid {len(img)} bytes of {os.path.basename(floppy)} at LBA 0")
