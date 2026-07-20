#!/usr/bin/env python3
"""trim-wad.py - make a small single-level IWAD for testing Duum.

The full IWAD (freedoom1.wad is ~28 MB) is fine on real hardware where Duum
streams it with uno.read_at, but QEMU's vvfat corrupts writes into a large
image, which breaks Studio's on-device build-and-run test path.  This writes a
tiny IWAD containing just one level's lumps (+ the palette), so the test ESP
stays small.  The engine reads it identically - same directory, same read_at.

  python3 tools/trim-wad.py wads/DOOM1.WAD wads/E1M1.WAD [E1M1]
"""
import struct, sys

MAPSUB = ("THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS",
          "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP")
EXTRA = ("PLAYPAL", "COLORMAP")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    level = sys.argv[3] if len(sys.argv) > 3 else "E1M1"
    d = open(src, "rb").read()
    magic, n, diro = struct.unpack_from("<4sII", d, 0)
    direntry = []
    for i in range(n):
        off, sz, nm = struct.unpack_from("<II8s", d, diro + 16 * i)
        direntry.append((nm.rstrip(b"\0").decode("latin1"), off, sz))
    names = [e[0] for e in direntry]

    keep = []
    # the level marker + its ten map lumps, in order
    li = names.index(level)
    keep.append(direntry[li])
    for sub in MAPSUB:
        for j in range(li + 1, li + 12):
            if direntry[j][0] == sub:
                keep.append(direntry[j]); break
    # palette + colormap (small, wanted later for textures)
    for ex in EXTRA:
        if ex in names:
            keep.append(direntry[names.index(ex)])

    # rebuild a compact IWAD: header, then lump data, then the directory
    out = bytearray(12)
    entries = []
    for nm, off, sz in keep:
        data = d[off:off + sz]
        entries.append((nm, len(out), len(data)))
        out += data
    dirpos = len(out)
    for nm, off, sz in entries:
        out += struct.pack("<II8s", off, sz, nm.encode("latin1")[:8])
    struct.pack_into("<4sII", out, 0, b"IWAD", len(entries), dirpos)
    open(dst, "wb").write(out)
    print("wrote %s: %d lumps, %d bytes (%s)" %
          (dst, len(entries), len(out), ", ".join(e[0] for e in entries)))


if __name__ == "__main__":
    main()
