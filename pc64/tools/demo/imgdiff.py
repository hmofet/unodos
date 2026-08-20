"""imgdiff.py - minimal PNG reader + view-region pixel diff for the zgrab PNGs.

Enough to compare two zgrab PNGs without a Pillow dependency, so a metal
gate can tell 'the player stopped' from 'the player kept going'.
"""
import struct, zlib

X0, X1, Y0, Y1 = 54, 570, 58, 385          # the 3D view, above the status bar


def read_png(path):
    d = open(path, "rb").read()
    pos, idat, w, h, nch = 8, b"", 0, 0, 3
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        tag = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, _dep, col = struct.unpack(">IIBB", body[:10])
            nch = 3 if col == 2 else 4
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride, out, prev, p = w * nch, bytearray(), bytearray(w * nch), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    return w, nch, bytes(out)


def diff(p1, p2):
    w1, n1, a = read_png(p1)
    w2, n2, b = read_png(p2)
    d = t = 0
    cols = []
    for y in range(Y0, Y1):
        r1, r2 = y * w1 * n1, y * w2 * n2
        for x in range(X0, X1):
            t += 1
            if a[r1 + x * n1:r1 + x * n1 + 3] != b[r2 + x * n2:r2 + x * n2 + 3]:
                d += 1
                cols.append(x)
    span = (max(cols) - min(cols)) if cols else 0
    return 100.0 * d / t, span


