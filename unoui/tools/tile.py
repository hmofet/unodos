#!/usr/bin/env python3
"""Tile PPM renders into one contact-sheet PNG (stdlib only, same minimal-PNG
approach as ppm2png.py). Each render is cropped to a region (the harness draws a
caption strip at the top of every frame, so no external legend is needed) and
laid out in a grid.

  tile.py out.png cols [x,y,w,h] in0.ppm in1.ppm ...

The optional x,y,w,h (a single comma-separated arg) overrides the default crop.
"""
import sys, zlib, struct


def read_ppm(path):
    data = open(path, "rb").read()
    assert data[:2] == b"P6", path + ": not a P6 PPM"
    idx, vals = 2, []
    while len(vals) < 3:
        while data[idx] in b" \t\n\r":
            idx += 1
        start = idx
        while data[idx] not in b" \t\n\r":
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, _ = vals
    idx += 1
    return w, h, data[idx:idx + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + \
            struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


# default crop: keep the caption strip + the dialog window
CX, CY, CW, CH = 110, 0, 420, 380
GAP = 6
BG = (40, 40, 48)


def crop(w, h, rgb):
    out = bytearray(CW * CH * 3)
    for y in range(CH):
        sy = CY + y
        if sy >= h:
            break
        srow = (sy * w + CX) * 3
        drow = y * CW * 3
        for x in range(CW):
            sx = CX + x
            if sx >= w:
                break
            out[drow + x*3:drow + x*3 + 3] = rgb[srow + x*3:srow + x*3 + 3]
    return out


def main():
    global CX, CY, CW, CH
    out_png = sys.argv[1]
    cols = int(sys.argv[2])
    rest = sys.argv[3:]
    if rest and "," in rest[0]:
        CX, CY, CW, CH = (int(v) for v in rest[0].split(","))
        rest = rest[1:]
    paths = rest
    tiles = [crop(*read_ppm(p)) for p in paths]
    rows = (len(tiles) + cols - 1) // cols
    W = cols * CW + (cols + 1) * GAP
    H = rows * CH + (rows + 1) * GAP
    sheet = bytearray(BG * (W * H))
    for i, tile in enumerate(tiles):
        c, r = i % cols, i // cols
        ox = GAP + c * (CW + GAP)
        oy = GAP + r * (CH + GAP)
        for y in range(CH):
            d = ((oy + y) * W + ox) * 3
            s = y * CW * 3
            sheet[d:d + CW * 3] = tile[s:s + CW * 3]
    write_png(out_png, W, H, bytes(sheet))
    print("wrote %s (%dx%d, %d themes %dx%d)" % (out_png, W, H, len(tiles), cols, rows))


if __name__ == "__main__":
    main()
