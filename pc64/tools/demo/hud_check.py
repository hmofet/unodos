#!/usr/bin/env python3
"""hud_check - prove the red perf HUD is (or is not) in a captured frame.

The debug build paints a red HUD in the TOP-RIGHT corner, and `nohud` in
DEBUG.CFG turns it off (pc64/DEBUG.md). "It looks gone" is not evidence on a
frame that is mostly pale UI, so this counts strongly-red pixels in the
top-right band and prints the number.

    python3 hud_check.py frame.png [frame.png ...]

A HUD-on frame scores in the hundreds-to-thousands; a HUD-off frame should be
essentially zero. Exit 1 if any frame is over --max.
"""
import argparse, struct, sys, zlib


def read_png(path):
    """Minimal PNG reader for the RGB/RGBA, 8-bit, non-interlaced files ffmpeg
    writes. Enough to count pixels without a Pillow dependency."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos, idat, w = 8, b"", None
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour not in (2, 6):
                raise ValueError("unsupported PNG (depth %d colour %d)"
                                 % (depth, colour))
            nch = 3 if colour == 2 else 4
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(w * h * nch)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if filt == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, nch, out


def hud_red(path, band_w=0.42, band_h=0.06):
    """Count strongly-red pixels in the top-right band, where the HUD lives."""
    w, h, nch, px = read_png(path)
    x0, y1 = int(w * (1.0 - band_w)), max(1, int(h * band_h))
    n = 0
    for y in range(y1):
        ro = y * w * nch
        for x in range(x0, w):
            o = ro + x * nch
            r, g, b = px[o], px[o + 1], px[o + 2]
            if r > 140 and g < 90 and b < 90 and r - g > 70 and r - b > 70:
                n += 1
    return n, w, h


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--max", type=int, default=20,
                    help="fail above this many red pixels (default 20)")
    a = ap.parse_args(argv)
    bad = 0
    for f in a.frames:
        n, w, h = hud_red(f)
        verdict = "HUD-FREE" if n <= a.max else "HUD PRESENT"
        print("%-52s %5d red px in the top-right band (%dx%d)  %s"
              % (f, n, w, h, verdict))
        if n > a.max:
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
