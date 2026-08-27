#!/usr/bin/env python3
"""Wrap a flat AArch64 payload in an Android boot image (AOSP header v1) that Planet's
LK will load as the "kernel" and enter per the arm64 boot protocol. The header fields
match the stock Cosmo v23 boot.img (research/COSMO-BRINGUP.md): kernel_addr 0x40080000,
tags 0x54000000, page_size 2048. No ramdisk / second / dtb — LK supplies the DTB in x0.

Usage: mkbootimg.py <payload.bin> <out-boot.img> [--cmdline "..."]
"""
import struct, sys

BASE          = 0x40000000
KERNEL_OFFSET = 0x00080000     # -> kernel_addr 0x40080000
RAMDISK_OFF   = 0x15000000     # -> 0x55000000 (unused)
SECOND_OFF    = 0x00f00000     # -> 0x40f00000 (unused)
TAGS_OFF      = 0x14000000     # -> tags_addr 0x54000000 (LK puts the DTB here)
PAGE_SIZE     = 2048
HDR_VERSION   = 1
OS_VERSION    = 0x12000144     # os_version|patch, copied from stock v23
DEFAULT_CMDLINE = "bootopt=64S3,32N2,64N2"

def pad(n, page):
    r = n % page
    return b"" if r == 0 else b"\x00" * (page - r)

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    payload_path, out_path = sys.argv[1], sys.argv[2]
    cmdline = DEFAULT_CMDLINE
    if "--cmdline" in sys.argv:
        cmdline = sys.argv[sys.argv.index("--cmdline") + 1]

    with open(payload_path, "rb") as f:
        kernel = f.read()

    hdr = bytearray(PAGE_SIZE)
    struct.pack_into("<8s", hdr, 0, b"ANDROID!")
    struct.pack_into("<II", hdr, 8,  len(kernel), BASE + KERNEL_OFFSET)
    struct.pack_into("<II", hdr, 16, 0,           BASE + RAMDISK_OFF)   # ramdisk
    struct.pack_into("<II", hdr, 24, 0,           BASE + SECOND_OFF)    # second
    struct.pack_into("<IIII", hdr, 32,
                     BASE + TAGS_OFF, PAGE_SIZE, HDR_VERSION, OS_VERSION)
    # 48: product name (16, blank). 64: cmdline (512). 576: id (32). 608: extra (1024).
    cb = cmdline.encode("latin1")[:511]
    hdr[64:64 + len(cb)] = cb
    # v1 fields: recovery_dtbo_size/offset + header_size at 0x660.
    struct.pack_into("<IQ", hdr, 0x660, 0, 0)      # recovery_dtbo size/offset
    struct.pack_into("<I",  hdr, 0x66c, 0x660)     # header_size (v1)

    with open(out_path, "wb") as f:
        f.write(hdr)                    # header page (already PAGE_SIZE)
        f.write(kernel)
        f.write(pad(len(kernel), PAGE_SIZE))

    total = PAGE_SIZE + len(kernel) + len(pad(len(kernel), PAGE_SIZE))
    print(f"mkbootimg: {out_path} ({total} bytes; kernel {len(kernel)} @ "
          f"0x{BASE+KERNEL_OFFSET:08x}, tags 0x{BASE+TAGS_OFF:08x})")
    if total > 32 * 1024 * 1024:
        print("WARNING: image exceeds the 32 MiB p42 slot ceiling!", file=sys.stderr)

if __name__ == "__main__":
    main()
