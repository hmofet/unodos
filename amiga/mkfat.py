#!/usr/bin/env python3
"""Build an 880KB FAT12 data disk for the UnoDOS Amiga port.

Geometry is the native Amiga DD layout (512-byte sectors, 11 per track,
2 heads, 80 cylinders = 1760 sectors), written as a plain 901120-byte
image that WinUAE serves as DF1 and the trackdisk driver reads with
standard Amiga MFM DMA. The *filesystem* is plain FAT12, so the image is
PC-interchangeable at the file level: `mtools` reads/writes it directly
(add to ~/.mtoolsrc:  drive u: file="unodos-data.adf"  mformat_only).

Usage: python mkfat.py <out.img> [file1 file2 ...]
       (run from the repo root; defaults to amiga/disk/*.TXT)
"""
import glob, os, struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "amiga/build/unodos-data.adf"
files = sys.argv[2:] or sorted(glob.glob("amiga/disk/*.TXT"))

BPS = 512               # bytes per sector
SPT = 11                # sectors per track (Amiga DD)
HEADS = 2
CYLS = 80
TOTAL = SPT * HEADS * CYLS          # 1760 sectors
SPC = 2                 # sectors per cluster -> 1KB clusters
RESERVED = 1
NFATS = 2
SPF = 3                 # sectors per FAT (1760/2 clusters * 1.5B = ~1.3KB)
ROOT_ENTRIES = 112      # 7 sectors of root directory... 112*32=3584=7 sect

root_secs = ROOT_ENTRIES * 32 // BPS
data_start = RESERVED + NFATS * SPF + root_secs
clusters = (TOTAL - data_start) // SPC

img = bytearray(TOTAL * BPS)

# ---- boot sector / BPB
bpb = bytearray(BPS)
bpb[0:3] = b"\xEB\x3C\x90"
bpb[3:11] = b"UNODOS3 "
struct.pack_into("<H", bpb, 11, BPS)
bpb[13] = SPC
struct.pack_into("<H", bpb, 14, RESERVED)
bpb[16] = NFATS
struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)
struct.pack_into("<H", bpb, 19, TOTAL)
bpb[21] = 0xF9                      # media descriptor (DD)
struct.pack_into("<H", bpb, 22, SPF)
struct.pack_into("<H", bpb, 24, SPT)
struct.pack_into("<H", bpb, 26, HEADS)
bpb[38] = 0x29
bpb[43:54] = b"UNODOS-DATA"
bpb[54:62] = b"FAT12   "
bpb[510] = 0x55
bpb[511] = 0xAA
img[0:BPS] = bpb

# ---- FAT (12-bit) helpers
fat = bytearray(SPF * BPS)
def fat_set(cl, val):
    off = cl * 3 // 2
    if cl & 1:
        fat[off] = (fat[off] & 0x0F) | ((val << 4) & 0xF0)
        fat[off + 1] = (val >> 4) & 0xFF
    else:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
fat_set(0, 0xFF9)
fat_set(1, 0xFFF)

# ---- root dir + data
rootdir = bytearray(root_secs * BPS)
next_cluster = 2
dirslot = 0

def put_file(name, data):
    global next_cluster, dirslot
    base, _, ext = name.upper().partition(".")
    fname = (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")
    nclust = max(1, (len(data) + SPC * BPS - 1) // (SPC * BPS))
    first = next_cluster
    for i in range(nclust):
        cl = next_cluster + i
        fat_set(cl, 0xFFF if i == nclust - 1 else cl + 1)
        # write data
        lba = data_start + (cl - 2) * SPC
        chunk = data[i * SPC * BPS:(i + 1) * SPC * BPS]
        img[lba * BPS:lba * BPS + len(chunk)] = chunk
    next_cluster += nclust
    e = dirslot * 32
    rootdir[e:e + 11] = fname
    rootdir[e + 11] = 0x20           # archive
    struct.pack_into("<H", rootdir, e + 26, first)
    struct.pack_into("<I", rootdir, e + 28, len(data))
    dirslot += 1
    print(f"  {fname.decode()} {len(data)} bytes, cluster {first}")

for f in files:
    data = open(f, "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r")
    put_file(os.path.basename(f), data)

# a longer test file to exercise multi-cluster FAT chains
big = ("UnoDOS FAT12 chain test.\r" * 100).encode("ascii")
put_file("CHAIN.TXT", big)

# ---- assemble
for n in range(NFATS):
    s = RESERVED + n * SPF
    img[s * BPS:(s + SPF) * BPS] = fat
s = RESERVED + NFATS * SPF
img[s * BPS:s * BPS + len(rootdir)] = rootdir

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(img)
print(f"wrote {OUT}: {TOTAL} sectors, {dirslot} files, "
      f"{clusters} clusters of {SPC * BPS}B, data starts LBA {data_start}")
