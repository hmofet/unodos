#!/usr/bin/env python3
"""Convert a 140K DOS-3.3-order .dsk into a .woz (WOZ 2.0) and .nib image.

Why: the harness verifies UnoDOS on the ROM-free py65 rig, but BMOW's
FloppyEmu on real hardware (and the Apple IIc in particular) is happiest
with a .woz - it is self-describing (declares disk type 5.25", 16-sector
boot) and carries real track timing + self-sync, so a picky/old firmware
or a timing-sensitive IIc won't reject it the way it can a bare .dsk.
A .nib is emitted too as a raw-nibble fallback.

The disk is STANDARD Apple II 6-and-2 GCR (std prologues D5 AA 96 /
D5 AA AD, std write-translate table, DOS 3.3 soft skew) - the GCR tables
below mirror boot.s exactly (same as harness.py's nibblizer).

Usage: mkwoz.py <in.dsk> <out_basename>   ->  <out_basename>.woz + .nib
"""
import sys, struct, zlib

TRACK = 4096
TRACKS = 35
DISK = TRACK * TRACKS

# --- GCR 6-and-2 tables - mirror boot.s / harness.py exactly --------------
WRTAB = [
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
]
FLIP2 = [0, 2, 1, 3]
SKEW = [0x0, 0x7, 0xE, 0x6, 0xD, 0x5, 0xC, 0x4,
        0xB, 0x3, 0xA, 0x2, 0x9, 0x1, 0x8, 0xF]


def nibblize_data(payload):
    """256-byte sector -> 343 GCR disk bytes (86 twos + 256 sixes + chk).
    Identical to harness.py.nibblize_data (the std DOS 3.3 diff chain)."""
    assert len(payload) == 256
    rdbuf2 = [0] * 86
    for j in range(86):
        bits10 = FLIP2[payload[j] & 3]
        bits32 = FLIP2[payload[j + 86] & 3]
        bits54 = FLIP2[payload[j + 172] & 3] if j + 172 <= 255 else 0
        rdbuf2[j] = (bits54 << 4) | (bits32 << 2) | bits10
    sixes = [b >> 2 for b in payload]
    out, prev = [], 0
    for j in range(85, -1, -1):
        v = rdbuf2[j]
        out.append(WRTAB[v ^ prev]); prev = v
    for v in sixes:
        out.append(WRTAB[v ^ prev]); prev = v
    out.append(WRTAB[prev])
    return out


def enc44(d):
    return ((d >> 1) | 0xAA, d | 0xAA)


def track_bytes(track, data4096):
    """Build one track's raw disk-byte (nibble) stream, std 16-sector
    layout with DOS 3.3 physical->logical skew. Returns (bytes, sync_flags)
    where sync_flags[i] marks a self-sync (0xFF) byte for WOZ 10-bit
    encoding. .nib just uses the bytes."""
    out, sync = [], []

    def put(b, is_sync=False):
        out.append(b); sync.append(is_sync)

    for phys in range(16):
        logical = SKEW[phys]
        payload = data4096[logical * 256:(logical + 1) * 256]
        for _ in range(16):
            put(0xFF, True)                     # gap 1
        for b in (0xD5, 0xAA, 0x96):
            put(b)
        for v in (0xFE, track, phys, 0xFE ^ track ^ phys):
            b1, b2 = enc44(v); put(b1); put(b2)
        for b in (0xDE, 0xAA, 0xEB):
            put(b)
        for _ in range(7):
            put(0xFF, True)                     # gap 2
        for b in (0xD5, 0xAA, 0xAD):
            put(b)
        for nb in nibblize_data(payload):
            put(nb)
        for b in (0xDE, 0xAA, 0xEB):
            put(b)
    for _ in range(20):
        put(0xFF, True)                         # gap 3 / track tail
    return out, sync


def bits_for_track(track, data4096):
    """MSB-first bit list. Data bytes = 8 bits; self-sync = 10 bits
    (the byte + two 0 cells), as Applesauce writes synthesized tracks."""
    nibs, sync = track_bytes(track, data4096)
    bits = []
    for b, is_sync in zip(nibs, sync):
        for i in range(7, -1, -1):
            bits.append((b >> i) & 1)
        if is_sync:
            bits.append(0); bits.append(0)
    return bits


def pack_bits(bits):
    """bit list -> bytes (MSB-first), zero-padded to a whole byte."""
    pad = (-len(bits)) % 8
    bits = bits + [0] * pad
    out = bytearray(len(bits) // 8)
    for i, bit in enumerate(bits):
        if bit:
            out[i >> 3] |= 0x80 >> (i & 7)
    return bytes(out)


def build_woz(dsk):
    BLOCK = 512
    # --- per-track bitstreams ---
    trk_blocks = []          # (start_block, block_count, bit_count, blob)
    blob_all = bytearray()
    start_block = 3          # bitstream data begins at block 3 (after
    #                          header+INFO+TMAP+TRKS-table; see layout below)
    largest = 0
    for t in range(TRACKS):
        bits = bits_for_track(t, dsk[t * TRACK:(t + 1) * TRACK])
        raw = pack_bits(bits)
        bc = (len(raw) + BLOCK - 1) // BLOCK
        raw = raw + b"\x00" * (bc * BLOCK - len(raw))
        trk_blocks.append((start_block, bc, len(bits)))
        blob_all += raw
        start_block += bc
        largest = max(largest, bc)

    # --- TMAP (160 quarter-track entries) ---
    tmap = bytearray([0xFF] * 160)
    for t in range(TRACKS):
        tmap[t * 4] = t
        if t * 4 - 1 >= 0:
            tmap[t * 4 - 1] = t
        if t * 4 + 1 < 160:
            tmap[t * 4 + 1] = t

    # --- INFO (60 bytes, WOZ v2) ---
    info = bytearray(60)
    info[0] = 2                                  # INFO version
    info[1] = 1                                  # disk type: 1 = 5.25"
    info[2] = 0                                  # write protected
    info[3] = 0                                  # synchronized
    info[4] = 0                                  # cleaned
    creator = "UnoDOS mkwoz".ljust(32)[:32].encode("ascii")
    info[5:37] = creator
    info[37] = 1                                 # disk sides
    info[38] = 1                                 # boot sector fmt: 16-sector
    info[39] = 32                                # optimal bit timing (4us)
    struct.pack_into("<H", info, 40, 0)          # compatible hardware
    struct.pack_into("<H", info, 42, 0)          # required RAM
    struct.pack_into("<H", info, 44, largest)    # largest track (blocks)
    # 46..59 reserved (zero)

    # --- TRKS: 160 x 8-byte TRK table, then bitstream blob ---
    trks = bytearray()
    bi = 0
    for t in range(160):
        if t < TRACKS:
            sb, bc, bitc = trk_blocks[t]
            trks += struct.pack("<HHI", sb, bc, bitc)
        else:
            trks += struct.pack("<HHI", 0, 0, 0)
    trks += blob_all

    def chunk(cid, payload):
        return cid + struct.pack("<I", len(payload)) + payload

    body = chunk(b"INFO", info) + chunk(b"TMAP", tmap) + chunk(b"TRKS", trks)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    header = b"WOZ2" + bytes([0xFF, 0x0A, 0x0D, 0x0A])
    return header + struct.pack("<I", crc) + body


def build_nib(dsk):
    """Raw-nibble image: 35 tracks x 6656 disk bytes, sync-padded."""
    NIBTRK = 6656
    out = bytearray()
    for t in range(TRACKS):
        nibs, _ = track_bytes(t, dsk[t * TRACK:(t + 1) * TRACK])
        if len(nibs) > NIBTRK:
            raise SystemExit(f"track {t} nibble stream {len(nibs)} > {NIBTRK}")
        nibs = nibs + [0xFF] * (NIBTRK - len(nibs))
        out += bytes(nibs)
    return bytes(out)


def main():
    in_p, base = sys.argv[1:3]
    dsk = open(in_p, "rb").read()
    if len(dsk) != DISK:
        raise SystemExit(f"{in_p}: expected {DISK} bytes (140K), got {len(dsk)}")
    woz = build_woz(dsk)
    nib = build_nib(dsk)
    open(base + ".woz", "wb").write(woz)
    open(base + ".nib", "wb").write(nib)
    print(f"{base}.woz: {len(woz)} bytes (WOZ 2.0, 5.25\", 35 tracks)")
    print(f"{base}.nib: {len(nib)} bytes (raw nibble, 35 x 6656)")


if __name__ == "__main__":
    main()
