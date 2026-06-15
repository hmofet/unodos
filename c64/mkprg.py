#!/usr/bin/env python3
"""Pack the assembled kernel into a C64 .prg and a bootable 1541 .d64.

dasm assembles kernel.s (org $0801) to a raw binary; this prepends the
little-endian load address ($0801) to make a standard CBM .prg, then writes
a 35-track .d64 disk image containing that program as a single PRG file
named "UNODOS" (so a real C64 / VICE loads it with  LOAD"UNODOS",8,1 : RUN
or  LOAD"*",8,1 : RUN ).

Usage: mkprg.py <kernel.bin> <out.prg> <out.d64>
"""
import sys

LOAD_ADDR = 0x0801

# 1541 geometry: sectors per track (1-indexed track number).
SPT = ([21] * 17) + ([19] * 7) + ([18] * 6) + ([17] * 5)   # tracks 1..35
DIR_TRACK = 18
INTERLEAVE = 10


def sectors_per_track(t):
    return SPT[t - 1]


def linear_offset(track, sector):
    off = 0
    for t in range(1, track):
        off += sectors_per_track(t)
    return (off + sector) * 256


def make_prg(raw):
    return bytes([LOAD_ADDR & 0xFF, (LOAD_ADDR >> 8) & 0xFF]) + raw


def make_d64(prg, name="UNODOS"):
    img = bytearray(683 * 256)            # 174848 bytes, 35-track no-error d64

    # ---- lay the PRG file out in a chain of sectors (skip the dir track) ----
    free = []
    for t in range(1, 36):
        if t == DIR_TRACK:
            continue
        for s in range(sectors_per_track(t)):
            free.append((t, s))
    # order sectors with the classic interleave so the chain reads fast
    chain = []
    nbytes = len(prg)
    nsec = (nbytes + 253) // 254
    used = set()
    t, s = 1, 0
    for _ in range(nsec):
        while (t, s) in used or t == DIR_TRACK:
            s += 1
            if s >= sectors_per_track(t):
                s = 0
                t += 1
                if t == DIR_TRACK:
                    t += 1
        chain.append((t, s))
        used.add((t, s))
        s = (s + INTERLEAVE) % sectors_per_track(t)

    for i, (t, s) in enumerate(chain):
        off = linear_offset(t, s)
        start = i * 254
        block = prg[start:start + 254]
        if i + 1 < len(chain):
            nt, ns = chain[i + 1]
            img[off] = nt
            img[off + 1] = ns
        else:
            img[off] = 0                  # last sector: link track 0
            img[off + 1] = len(block) + 1  # ptr to last used byte
        img[off + 2:off + 2 + len(block)] = block

    # ---- BAM (track 18 sector 0) ----
    bam = linear_offset(DIR_TRACK, 0)
    img[bam + 0] = DIR_TRACK              # first dir track
    img[bam + 1] = 1                      # first dir sector
    img[bam + 2] = 0x41                   # 'A' = DOS version
    img[bam + 3] = 0x00
    # per-track free map: 4 bytes/track (count + 3 bitmap bytes), tracks 1..35
    for t in range(1, 36):
        spt = sectors_per_track(t)
        bits = (1 << spt) - 1
        for s in range(spt):
            if (t, s) in used or (t == DIR_TRACK and s in (0, 1)):
                bits &= ~(1 << s)
        cnt = bin(bits).count("1")
        e = bam + 4 * t
        img[e + 0] = cnt
        img[e + 1] = bits & 0xFF
        img[e + 2] = (bits >> 8) & 0xFF
        img[e + 3] = (bits >> 16) & 0xFF
    # disk name (16, $A0-padded), id, dos type
    nm = name.encode("ascii")[:16]
    img[bam + 0x90:bam + 0xA0] = nm + b"\xA0" * (16 - len(nm))
    img[bam + 0xA0] = img[bam + 0xA1] = 0xA0
    img[bam + 0xA2] = ord('U')
    img[bam + 0xA3] = ord('1')
    img[bam + 0xA4] = 0xA0
    img[bam + 0xA5] = 0xA0
    img[bam + 0xA6:bam + 0xAB] = b"\xA0" * 5

    # ---- directory entry (track 18 sector 1, first 32-byte slot) ----
    d = linear_offset(DIR_TRACK, 1)
    img[d + 0] = 0                        # no next dir sector
    img[d + 1] = 0xFF
    e = d + 2
    img[e + 0] = 0x82                     # file type: closed PRG
    img[e + 1] = chain[0][0]             # first data track
    img[e + 2] = chain[0][1]             # first data sector
    img[e + 3:e + 0x13] = nm + b"\xA0" * (16 - len(nm))
    img[e + 0x1C] = len(chain) & 0xFF    # block count lo
    img[e + 0x1D] = (len(chain) >> 8) & 0xFF
    return bytes(img)


def main():
    binf, prgf, d64f = sys.argv[1], sys.argv[2], sys.argv[3]
    raw = open(binf, "rb").read()
    prg = make_prg(raw)
    open(prgf, "wb").write(prg)
    print("wrote %s (%d bytes, load $%04X-$%04X)" %
          (prgf, len(prg), LOAD_ADDR, LOAD_ADDR + len(raw) - 1))
    d64 = make_d64(prg)
    open(d64f, "wb").write(d64)
    print("wrote %s (%d bytes)" % (d64f, len(d64)))


if __name__ == "__main__":
    main()
