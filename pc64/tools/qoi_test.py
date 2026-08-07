#!/usr/bin/env python3
"""qoi_test - the kernel's QOI decoder round-trips, on the host.

    python3 tools/qoi_test.py

The decoder in pc64_qoi.c is what lets an app ship its own icon: the shell has
to draw that art before it would load a byte of the app's code, so the decoder
cannot be in a module. This compiles the KERNEL's copy of it with the host gcc,
feeds it what mkicon.py encoded, and compares pixel for pixel. The encoder and
the decoder are separate implementations in different languages, which is the
only reason a round trip proves anything.

Also checks the error paths, because a bad icon file must cost a plainer icon
and nothing else - never a crash, and never a read past the end of the buffer.
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkicon                                              # noqa: E402

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


tmp = tempfile.mkdtemp()
exe = os.path.join(tmp, "qoi_test")
cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-o", exe,
                     os.path.join(HERE, "qoi_test.c"),
                     os.path.join(HERE, "..", "pc64_qoi.c")],
                    capture_output=True)
if cc.returncode:
    print(cc.stderr.decode())
    sys.exit("qoi_test: the harness would not build")
check(not cc.stderr.strip(), "pc64_qoi.c compiles clean at -Wall -Wextra")


def roundtrip(w, h, px, label):
    blob = mkicon.qoi_encode(w, h, px)
    src = os.path.join(tmp, "in.qoi")
    dst = os.path.join(tmp, "out.raw")
    open(src, "wb").write(blob)
    r = subprocess.run([exe, src, dst], capture_output=True)
    if r.returncode != 0:
        check(False, "%s: decoded at all (%s)" % (label, r.stdout.decode().strip()))
        return
    gw, gh = (int(x) for x in r.stdout.split())
    got = open(dst, "rb").read()
    check((gw, gh) == (w, h), "%s: %dx%d survives" % (label, w, h))
    if got == px:
        check(True, "%s: every pixel identical (%d bytes)" % (label, len(px)))
    else:
        bad = sum(1 for i in range(0, min(len(got), len(px)), 4)
                  if got[i:i+4] != px[i:i+4])
        check(False, "%s: %d pixels differ" % (label, bad))


# 1. the shipped demo emblem: flat runs, an index hit, hard colour steps
w, h, px = mkicon.demo()
roundtrip(w, h, px, "the demo emblem")

# 2. a gradient: exercises DIFF and LUMA, which the flat art never reaches
px = bytearray(32 * 32 * 4)
for y in range(32):
    for x in range(32):
        o = (y * 32 + x) * 4
        px[o:o+4] = bytes((x * 8 % 256, y * 8 % 256, (x + y) * 4 % 256,
                           255 if (x + y) % 7 else 0))
roundtrip(32, 32, bytes(px), "a gradient with holes")

# 3. one solid colour: the whole image is one long run
px = bytes([17, 99, 200, 255] * (32 * 32))
roundtrip(32, 32, px, "a single flat colour")

# ---- the error paths ------------------------------------------------------
def rejects(blob, label):
    src = os.path.join(tmp, "bad.qoi")
    open(src, "wb").write(blob)
    r = subprocess.run([exe, src, os.path.join(tmp, "bad.raw")],
                       capture_output=True, timeout=20)
    check(r.returncode == 1 and b"decode failed" in r.stdout,
          "refuses %s" % label)


good = mkicon.qoi_encode(*mkicon.demo())
rejects(b"", "an empty file")
rejects(b"not a qoi file at all, but long enough to be one" * 2, "wrong magic")
rejects(b"qoif" + struct.pack(">IIBB", 64, 64, 4, 0) + good[14:],
        "an image bigger than the 32x32 slot")
rejects(b"qoif" + struct.pack(">IIBB", 0, 32, 4, 0) + good[14:],
        "a zero dimension")
rejects(b"qoif" + struct.pack(">IIBB", 32, 32, 2, 0) + good[14:],
        "an impossible channel count")
rejects(good[:40], "a file that stops mid-stream")

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)
