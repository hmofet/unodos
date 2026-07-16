#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG using only the stdlib (zlib), so the host
shim's framebuffer dumps are viewable without PIL. Same minimal-PNG approach
as the other ports' harnesses.

  ppm2png.py in.ppm out.png
"""
import sys, zlib, struct


def read_ppm(path):
    data = open(path, "rb").read()
    assert data[:2] == b"P6", "not a P6 PPM"
    # parse header: P6 <w> <h> <maxval>, whitespace-separated, then one byte
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx] not in b"\n":
                idx += 1
            continue
        start = idx
        while data[idx] not in b" \t\n\r":
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, _ = vals
    idx += 1  # single whitespace after maxval
    return w, h, data[idx:idx + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter type 0
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))  # 8-bit RGB
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    w, h, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], w, h, rgb)
    print("wrote %s (%dx%d)" % (sys.argv[2], w, h))
