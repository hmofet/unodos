#!/usr/bin/env python3
# make_fat16_vhd.py - Wrap the DOS-style FAT16 HD image (build/unodos-hd.img,
# from tools/create_hd_image.py at geometry 615/4/26) as a fixed VHD for
# MartyPC's XT-IDE controller, so an 8088 can boot a standard, DOS-
# interchangeable FAT16 CompactFlash.
#
# MartyPC's XT-IDE only mounts VHDs whose geometry is in its supported drive-
# type table, so we overlay the raw image onto a copy of the bundled
# default_xtide.vhd (615/4/26, 63960 sectors) and keep its 512-byte footer.
import sys, os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
XT   = r"C:\Users\arin\xt-tools"

src      = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "unodos-hd.img")
template = sys.argv[2] if len(sys.argv) > 2 else os.path.join(XT, "media", "hdds", "default_xtide.vhd")
out      = sys.argv[3] if len(sys.argv) > 3 else os.path.join(XT, "media", "hdds", "unodos-fat16-cf.vhd")

with open(src, "rb") as f:
    data = f.read()
if not os.path.exists(template):
    sys.exit(f"template VHD not found: {template}")
shutil.copyfile(template, out)
size = os.path.getsize(out)
if len(data) > size - 512:
    sys.exit(f"FAT16 image ({len(data)}) too big for VHD data area ({size-512})")

with open(out, "r+b") as f:
    f.seek(0)
    f.write(data)
print(f"wrote {out} ({size} bytes); overlaid {len(data)} bytes of {os.path.basename(src)} at LBA 0")
