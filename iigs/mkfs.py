#!/usr/bin/env python3
"""Write the UnoDOS FAT12 filesystem into a packed IIGS boot disk.

The 800K boot image already holds the boot blocks (sectors 0-1) and the
kernel image (from byte 1024). This appends a self-contained FAT12 volume
starting at FS_START_SECTOR -- the same place the kernel's blk_io reads it
from via the ROM .Sony driver. The volume is plain FAT12, PC-interchangeable
(mtools / the x86 port's dump_fat12.py read it directly), with the geometry
the 68K FAT core (fat12.i) assumes: 1KB clusters (2 sectors), 3 sectors/FAT.

Usage: mkfs.py <disk.dsk> [file1 file2 ...]
       (defaults to macplus/disk/*.TXT, run from anywhere)
"""
import glob, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))

BPS = 512
DISK_SECTORS = 819200 // BPS        # 1600 (800K)
FS_START_SECTOR = 256               # sync: sony.i FS_START_SECTOR
TOTAL = DISK_SECTORS - FS_START_SECTOR  # 1344 sectors in the FAT12 volume

SPC = 2                 # sectors per cluster -> 1KB clusters (fat12.i assumes)
RESERVED = 1
NFATS = 2
SPF = 3                 # sectors per FAT
ROOT_ENTRIES = 112      # 112*32 = 3584 = 7 sectors of root directory

root_secs = ROOT_ENTRIES * 32 // BPS
data_start = RESERVED + NFATS * SPF + root_secs
clusters = (TOTAL - data_start) // SPC


def build_volume(files):
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
    struct.pack_into("<H", bpb, 24, 18)
    struct.pack_into("<H", bpb, 26, 2)
    bpb[38] = 0x29
    struct.pack_into("<L", bpb, 39, 0x554E4F44)
    bpb[43:54] = b"UNODOS-2GS "
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

    rootdir = bytearray(root_secs * BPS)
    state = {"cl": 2, "slot": 0}

    def put_file(name, data):
        base, _, ext = name.upper().partition(".")
        fname = (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")
        nclust = max(1, (len(data) + SPC * BPS - 1) // (SPC * BPS))
        first = state["cl"]
        for i in range(nclust):
            cl = state["cl"] + i
            fat_set(cl, 0xFFF if i == nclust - 1 else cl + 1)
            lba = data_start + (cl - 2) * SPC
            chunk = data[i * SPC * BPS:(i + 1) * SPC * BPS]
            img[lba * BPS:lba * BPS + len(chunk)] = chunk
        state["cl"] += nclust
        e = state["slot"] * 32
        rootdir[e:e + 11] = fname
        rootdir[e + 11] = 0x20           # archive
        struct.pack_into("<H", rootdir, e + 26, first)
        struct.pack_into("<I", rootdir, e + 28, len(data))
        state["slot"] += 1
        print(f"  {fname.decode()} {len(data)} bytes, cluster {first}")

    for spec in files:
        # "path" or "path:FATNAME" (e.g. build/demo.bin:DEMO.APP). Without an
        # override the FAT name comes from the basename.
        if ":" in spec and not os.path.exists(spec):
            path, _, name = spec.rpartition(":")
        else:
            path, name = spec, os.path.basename(spec)
        data = open(path, "rb").read()
        if name.upper().endswith(".TXT"):
            # text files use bare CR line endings (the apps split on $0D)
            data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
        put_file(name, data)

    # a multi-cluster file to exercise FAT chain walking
    big = ("UnoDOS FAT12 chain test on the Mac.\r" * 60).encode("ascii")
    put_file("CHAIN.TXT", big)

    for n in range(NFATS):
        s = RESERVED + n * SPF
        img[s * BPS:(s + SPF) * BPS] = fat
    s = RESERVED + NFATS * SPF
    img[s * BPS:s * BPS + len(rootdir)] = rootdir
    return img, state["slot"]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    dsk = sys.argv[1]
    files = sys.argv[2:] or sorted(glob.glob(os.path.join(HERE, "disk", "*.TXT")))

    disk = bytearray(open(dsk, "rb").read())
    assert len(disk) == 819200, f"expected 800K image, got {len(disk)}"
    kern_end = 1024  # boot blocks; the kernel lives right after, well under FS
    fs_off = FS_START_SECTOR * BPS
    assert kern_end <= fs_off, "kernel would overlap the filesystem"

    vol, nfiles = build_volume(files)
    disk[fs_off:fs_off + len(vol)] = vol
    open(dsk, "wb").write(disk)
    print(f"{dsk}: FAT12 volume at sector {FS_START_SECTOR} "
          f"({TOTAL} sectors, {nfiles} files, {clusters} clusters of "
          f"{SPC * BPS}B, data LBA {data_start})")


if __name__ == "__main__":
    main()
