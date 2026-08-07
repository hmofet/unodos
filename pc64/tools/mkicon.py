#!/usr/bin/env python3
# ===========================================================================
# mkicon.py - author a 32x32 QOI app icon.
#
#   python3 tools/mkicon.py <in.png|in.ppm|-> <out.QOI>
#   python3 tools/mkicon.py --demo <out.QOI>      a built-in sample emblem
#
# An app that ships from disk should be able to ship its own artwork, and the
# shell has to draw that artwork BEFORE it would load a byte of the app's code -
# so the decoder lives in the kernel (pc64_qoi.c) and the format is QOI, which
# this project already encodes for remote desktop. Put the file beside the
# module and name it in the descriptor:
#
#     UNO_APP_DESC("id: myapp\n" "icon: file:MYAPP.QOI\n");
#
# 32x32 RGBA. Alpha is a 1-bit key at runtime (>= 128 is opaque), so design for
# hard edges rather than a soft shadow.
# ===========================================================================
import struct, sys


def qoi_encode(w, h, px):
    """px: bytes, w*h*4 RGBA."""
    out = bytearray(b"qoif" + struct.pack(">IIBB", w, h, 4, 0))
    idx = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    n = w * h
    for i in range(n):
        p = tuple(px[i * 4: i * 4 + 4])
        if p == prev:
            run += 1
            if run == 62 or i == n - 1:
                out.append(0xC0 | (run - 1)); run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1)); run = 0
        h_ = (p[0] * 3 + p[1] * 5 + p[2] * 7 + p[3] * 11) & 63
        if idx[h_] == p:
            out.append(h_)
        else:
            idx[h_] = p
            if p[3] == prev[3]:
                dr, dg, db = (p[0] - prev[0] + 128) % 256 - 128, \
                             (p[1] - prev[1] + 128) % 256 - 128, \
                             (p[2] - prev[2] + 128) % 256 - 128
                dgr, dgb = dr - dg, db - dg
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
                elif -32 <= dg <= 31 and -8 <= dgr <= 7 and -8 <= dgb <= 7:
                    out.append(0x80 | (dg + 32))
                    out.append(((dgr + 8) << 4) | (dgb + 8))
                else:
                    out.append(0xFE); out += bytes(p[:3])
            else:
                out.append(0xFF); out += bytes(p)
        prev = p
    out += b"\0" * 7 + b"\x01"
    return bytes(out)


def demo():
    """A 32x32 emblem: a rounded slab with a bright bar across it. Deliberately
    unlike anything pc64_icons.c draws, so a screenshot shows at a glance that
    the art came from the FILE and not from the kernel's own vocabulary."""
    w = h = 32
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            inside = 3 <= x < 29 and 4 <= y < 28
            corner = ((x - 5) ** 2 + (y - 6) ** 2 < 4 and x < 5 and y < 6)
            if not inside or corner:
                px[o:o + 4] = bytes((0, 0, 0, 0))
                continue
            if 12 <= y < 18:
                px[o:o + 4] = bytes((250, 190, 60, 255))      # the bar
            elif y < 12:
                px[o:o + 4] = bytes((40, 110, 200, 255))      # top
            else:
                px[o:o + 4] = bytes((30, 80, 150, 255))       # bottom
    return w, h, bytes(px)


def read_ppm(path):
    d = open(path, "rb").read()
    if not d.startswith(b"P6"):
        sys.exit("mkicon: only P6 PPM or --demo (no PNG decoder here)")
    f = d.split(None, 4)
    w, h = int(f[1]), int(f[2])
    body = f[4]
    px = bytearray(w * h * 4)
    for i in range(w * h):
        px[i * 4: i * 4 + 3] = body[i * 3: i * 3 + 3]
        px[i * 4 + 3] = 255
    return w, h, bytes(px)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--demo":
        w, h, px = demo()
        out = sys.argv[2]
    elif len(sys.argv) == 3:
        w, h, px = read_ppm(sys.argv[1])
        out = sys.argv[2]
    else:
        sys.exit(__doc__ or "usage: mkicon.py <in.ppm>|--demo <out.QOI>")
    if w > 32 or h > 32:
        sys.exit("mkicon: %dx%d is larger than the 32x32 the shell decodes"
                 % (w, h))
    blob = qoi_encode(w, h, px)
    open(out, "wb").write(blob)
    print("mkicon: %s  %dx%d  %d bytes" % (out, w, h, len(blob)))
