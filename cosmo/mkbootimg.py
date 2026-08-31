#!/usr/bin/env python3
r"""Wrap a flat AArch64 payload in an Android boot image (AOSP header v1) that Planet's
LK will load as the "kernel" and enter per the arm64 boot protocol.

LK's 64-bit boot path is picky in three ways that a naive raw-payload image fails, all
read out of the archived LK source and confirmed against the stock v23 boot.img:

  1. THE PAYLOAD MUST BE GZIPPED. app/mt_boot/mt_boot.c:712 calls decompress_kernel()
     unconditionally on the 64-bit path -- there is no "is it compressed?" test. A raw
     arm64 Image gets "decompress kernel image fail!!!" and LK spins in while(1).
  2. A DEVICE TREE MUST BE APPENDED after it. app/mt_boot/fdt_op.c:373-395 reads the
     last DTB_MAX_SIZE bytes of the kernel region and scans BACKWARDS for d00dfeed;
     with no hit it gives up with "can't find dtb". LK then fixes that tree up (this is
     where atag,videolfb lands) and hands it to us in x0, so the tree we append is the
     tree our own fb_dtb_scan will walk.
  3. THE KERNEL REGION MUST NOT BE SMALLER THAN DTB_MAX_SIZE - page_size. That scan
     computes `offset = page_sz + kernel_sz - DTB_MAX_SIZE` (= 512 KB, mt_boot.h:39)
     and reads from it; for a small payload that goes negative. So we pad.

Layout, matching the stock image (gzip at kernel+0, tree near the end):

    [ 2048-byte header ][ gzip(payload) ][ zero pad ][ device tree ]
                        \___________________ kernel_size _________/

The device tree is the stock Cosmo one extracted from v23 firmware. It is a VENDOR
BLOB and is deliberately NOT committed to this repo -- point --dtb or $COSMO_DTB at
the copy in the research repo (hmofet/cosmo, analysis/v23/cosmo-boot.dtb).

Usage: mkbootimg.py <payload.bin> <out-boot.img> [--dtb PATH] [--cmdline "..."]
"""
import gzip, os, struct, sys

BASE          = 0x40000000
KERNEL_OFFSET = 0x00080000     # -> kernel_addr 0x40080000
RAMDISK_OFF   = 0x15000000     # -> 0x55000000 (unused)
SECOND_OFF    = 0x00f00000     # -> 0x40f00000 (unused)
TAGS_OFF      = 0x14000000     # -> tags_addr 0x54000000 (LK puts the DTB here)
PAGE_SIZE     = 2048
HDR_VERSION   = 1
OS_VERSION    = 0x12000144     # os_version|patch, copied from stock v23
DEFAULT_CMDLINE = "bootopt=64S3,32N2,64N2"

DTB_MAX_SIZE  = 512 * 1024     # LK app/mt_boot/mt_boot.h:39
MIN_KERNEL_SZ = DTB_MAX_SIZE - PAGE_SIZE   # keeps LK's backward-scan offset >= 0
FDT_MAGIC     = 0xD00DFEED

# The stock tree lives in the research repo, which is a separate checkout.
DTB_CANDIDATES = [
    os.environ.get("COSMO_DTB"),
    "C:/Repos/cosmo/analysis/v23/cosmo-boot.dtb",
    os.path.expanduser("~/cosmo/analysis/v23/cosmo-boot.dtb"),
]


def pad(n, page):
    r = n % page
    return b"" if r == 0 else b"\x00" * (page - r)


def find_dtb(explicit):
    for p in ([explicit] if explicit else DTB_CANDIDATES):
        if p and os.path.isfile(p):
            return p
    sys.exit(
        "mkbootimg: no device tree found.\n"
        "  LK refuses a boot image with no appended DTB (\"can't find dtb\"), so one\n"
        "  is required. It is a vendor blob and is not kept in this repo; pass\n"
        "  --dtb PATH or set COSMO_DTB to analysis/v23/cosmo-boot.dtb in the research\n"
        "  repo (hmofet/cosmo; see cosmo/HANDOFF.md for where that lives).\n"
        "  Looked in: " + ", ".join(p for p in DTB_CANDIDATES if p))


def read_dtb(path):
    with open(path, "rb") as f:
        dtb = f.read()
    if len(dtb) < 8 or struct.unpack(">I", dtb[:4])[0] != FDT_MAGIC:
        sys.exit("mkbootimg: %s is not a flattened device tree (bad magic)" % path)
    total = struct.unpack(">I", dtb[4:8])[0]
    if total > len(dtb):
        sys.exit("mkbootimg: %s is truncated (header says %d bytes, file has %d)"
                 % (path, total, len(dtb)))
    if total > DTB_MAX_SIZE:
        sys.exit("mkbootimg: %s is %d bytes, over LK's DTB_MAX_SIZE of %d"
                 % (path, total, DTB_MAX_SIZE))
    return dtb[:total]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    payload_path, out_path = sys.argv[1], sys.argv[2]
    cmdline = DEFAULT_CMDLINE
    if "--cmdline" in sys.argv:
        cmdline = sys.argv[sys.argv.index("--cmdline") + 1]
    explicit_dtb = None
    if "--dtb" in sys.argv:
        explicit_dtb = sys.argv[sys.argv.index("--dtb") + 1]

    with open(payload_path, "rb") as f:
        payload = f.read()

    dtb_path = find_dtb(explicit_dtb)
    dtb = read_dtb(dtb_path)
    # mtime=0 so the same payload always produces the same image.
    zimage = gzip.compress(payload, compresslevel=9, mtime=0)

    body = zimage + dtb
    padding = b""
    if len(body) < MIN_KERNEL_SZ:
        padding = b"\x00" * (MIN_KERNEL_SZ - len(body))
    kernel = zimage + padding + dtb        # tree LAST: LK scans backwards for it

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
    scan_off = PAGE_SIZE + len(kernel) - DTB_MAX_SIZE
    print("mkbootimg: %s (%d bytes)" % (out_path, total))
    print("    payload %d -> gzip %d, pad %d, dtb %d (%s)"
          % (len(payload), len(zimage), len(padding), len(dtb),
             os.path.basename(dtb_path)))
    print("    kernel %d @ 0x%08x, tags 0x%08x, LK dtb-scan offset %d"
          % (len(kernel), BASE + KERNEL_OFFSET, BASE + TAGS_OFF, scan_off))
    if scan_off < 0:
        print("WARNING: LK's dtb scan offset is negative; it will not find the tree",
              file=sys.stderr)
    if total > 32 * 1024 * 1024:
        print("WARNING: image exceeds the 32 MiB boot-slot ceiling!", file=sys.stderr)


if __name__ == "__main__":
    main()
