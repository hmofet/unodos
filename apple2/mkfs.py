#!/usr/bin/env python3
"""Format the UnoDOS/AppleII mini-FS ("USV1") into a packed .dsk and seed
disk/*.TXT files into it (run after mkdsk.py, the macplus mkfs.py pattern).

FS region: tracks FS_TRACK..34 (FS_TRACKS=15, FS_SECTORS=240 x 256B
sectors). A "rel sector" r maps to (track,logical) = (FS_TRACK+(r>>4),
r&$0F) - see fs.i - which is exact because FS_SECTORS=240=15*16, so the
whole region is flat at image offset FS_TRACK*4096 + r*256.

Catalog = rel sector 0: magic "USV1", file count, next-free rel sector
(heap grows up from 1), 15 x 16B directory entries (name[12] NUL-padded,
size.w LE, start.w LE).

Usage: mkfs.py <disk.dsk> [file ...]   (defaults to apple2/disk/*.TXT)
"""
import sys, glob, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))

TRACK = 4096
FS_TRACK = 20                  # sync: fs.i / mkdsk.py
FS_TRACKS = 35 - FS_TRACK
FS_SECTORS = FS_TRACKS * 16    # 240
FS_MAXFILES = 15

FSC_COUNT = 4
FSC_NEXTFREE = 5
FSC_DIR = 16
FSE_LEN = 16
FSE_SIZE = 12
FSE_START = 14

disk_p = sys.argv[1]
files = sys.argv[2:] or sorted(glob.glob(os.path.join(HERE, "disk", "*.TXT")))
assert len(files) <= FS_MAXFILES, f"{len(files)} seed files > FS_MAXFILES={FS_MAXFILES}"

img = bytearray(open(disk_p, "rb").read())
fs_off = FS_TRACK * TRACK

cat = bytearray(256)
cat[0:4] = b"USV1"

nextfree = 1
for i, path in enumerate(files):
    data = open(path, "rb").read()
    name = os.path.basename(path).encode("ascii")
    assert len(name) <= 12, f"{name!r} longer than 12 bytes"
    nsec = (len(data) + 255) // 256
    assert nextfree + nsec <= FS_SECTORS, "FS region full"
    ent = FSC_DIR + i * FSE_LEN
    cat[ent:ent + len(name)] = name
    struct.pack_into("<H", cat, ent + FSE_SIZE, len(data))
    struct.pack_into("<H", cat, ent + FSE_START, nextfree)
    off = fs_off + nextfree * 256
    img[off:off + len(data)] = data
    nextfree += nsec

cat[FSC_COUNT] = len(files)
cat[FSC_NEXTFREE] = nextfree
img[fs_off:fs_off + 256] = cat

open(disk_p, "wb").write(img)
print(f"{disk_p}: FS formatted, {len(files)} file(s), "
      f"{nextfree - 1}/{FS_SECTORS - 1} heap sectors used")
