#!/usr/bin/env python3
"""Generate a malformed-input fuzz corpus for the pc64 debug stress driver.

The deep review noted there is no malformed-input corpus anywhere, yet several
confirmed bugs are in parsers/decoders (MIDI program byte OOB, MP3, the image
w*h*4 alloc wrap, truncated/oversized markup).  This stages crafted-bad files
onto the ESP so the stress driver (and a human) can feed them through the real
decoders on metal:

  build/esp/STRESS/CORPUS/*.HTM,*.MD,*.TXT   text/markup parsers (shell path)
  build/esp/PICTURES/BAD_*.{PNG,JPG,GIF,BMP} image decoders (Photos.UNO)
  build/esp/MEDIA/BAD_*.{WAV,MID,MP3}         audio decoders (Music, in-kernel)

Every file is intentionally invalid in a specific way (truncated header, size
field that overflows a 32-bit product, out-of-range index, huge declared
length with no data).  A correct decoder rejects them cleanly; a buggy one
crashes and the fault handler files a report naming the decoder.

Usage: mkcorpus.py <esp-dir>
"""
import os, struct, sys

def w(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)

def main():
    esp = sys.argv[1]
    C = os.path.join(esp, "STRESS", "CORPUS")
    P = os.path.join(esp, "PICTURES")
    M = os.path.join(esp, "MEDIA")

    # ---- text / markup ----------------------------------------------------
    w(os.path.join(C, "TRUNC.HTM"), b"<html><body><table><tr><td>" * 3)  # unbalanced
    w(os.path.join(C, "HUGE.HTM"),  b"<p>" + b"A" * 400000 + b"</p>")     # oversized
    w(os.path.join(C, "NUL.MD"),    b"# head\x00\x00\x00\nbody\x00 with nuls\n")
    w(os.path.join(C, "DEEP.HTM"),  b"<div>" * 5000 + b"x" + b"</div>" * 5000)
    w(os.path.join(C, "BADUTF.TXT"), bytes(range(128, 256)) * 64)        # invalid UTF-8

    # ---- images -----------------------------------------------------------
    # PNG with a valid signature but IHDR declaring dimensions whose w*h*4
    # overflows 32 bits (the "Photos w*h*4 allocation wrap" class)
    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">II", 0x40000000, 0x40000000) + b"\x08\x06\x00\x00\x00"
    png += struct.pack(">I", len(ihdr)) + b"IHDR" + ihdr + b"\x00\x00\x00\x00"
    w(os.path.join(P, "BAD_WRAP.PNG"), png)
    # truncated JPEG (SOI then a marker claiming a huge segment length)
    w(os.path.join(P, "BAD_TRUNC.JPG"), b"\xff\xd8\xff\xe0\xff\xff" + b"\x00" * 8)
    # GIF with a logical screen far bigger than the (absent) pixel data
    w(os.path.join(P, "BAD_HUGE.GIF"),
      b"GIF89a" + struct.pack("<HH", 0xFFFF, 0xFFFF) + b"\xf7\x00\x00")
    # BMP with a bogus (negative-as-unsigned) width in the info header
    bmp = b"BM" + struct.pack("<IHHI", 54, 0, 0, 54)
    bmp += struct.pack("<IiiHHIIiiII", 40, 0x7FFFFFFF, 0x7FFFFFFF,
                       1, 32, 0, 0, 0, 0, 0, 0)
    w(os.path.join(P, "BAD_DIMS.BMP"), bmp)

    # ---- audio ------------------------------------------------------------
    # WAV whose data chunk claims far more bytes than the file holds
    wav = b"RIFF" + struct.pack("<I", 0xFFFFFF00) + b"WAVE"
    wav += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 176400, 4, 16)
    wav += b"data" + struct.pack("<I", 0xFFFFFF00) + b"\x00\x00\x00\x00"
    w(os.path.join(M, "BAD_LEN.WAV"), wav)
    # SMF (MIDI) with a program-change referencing an out-of-range program and
    # a running-status stream that runs past the declared track length (the
    # confirmed "MIDI program-byte OOB read")
    trk = b"\x00\xc0\x7f" + b"\x00\x90\x7f\x7f" * 8 + b"\x00\xff\x2f\x00"
    midi = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96)
    midi += b"MTrk" + struct.pack(">I", 0xFFFF) + trk           # length lies
    w(os.path.join(M, "BAD_PROG.MID"), midi)
    # MP3: a valid-looking frame header then nothing (bitrate/sr edge)
    w(os.path.join(M, "BAD_FRAME.MP3"), b"\xff\xfb\x90\x00" + b"\x00" * 4)

    sys.stderr.write("mkcorpus: staged fuzz corpus into %s\n" % esp)

if __name__ == "__main__":
    main()
