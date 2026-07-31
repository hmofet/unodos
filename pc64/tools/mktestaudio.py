#!/usr/bin/env python3
"""Generate test audio for UnoAmp: WAV, MOD and VGM.

Each file targets a specific claim the player makes, so a failure points at
something rather than just sounding wrong:

  TONE.WAV    440 Hz sine, 44.1 kHz stereo. If this is not in tune the sink's
              resampler is wrong, and nothing downstream can be trusted.
  SWEEP.WAV   20 Hz to 16 kHz over 8 seconds. Drives the spectrum analyser
              across every band in order - the bars should march left to right.
              A bar that never lights is a band-grouping bug in unoamp_vis.c.
  CHORD.WAV   Three steady sines (A2/E3/A3). Static spectrum: the same three
              bars should stay lit. Also the EQ test - move a slider and the
              matching bar must move.
  PAN.WAV     A tone hard left then hard right. Tests the balance control and
              proves the sink is not summing to mono.
  BEAT.MOD    A 4-channel ProTracker module. Exercises the phase 8 MOD player:
              sample looping, the tick clock, and Amiga hard panning.
  PSG.VGM     An SN76489 log. Exercises the VGM path and the PSG emulation,
              including the noise channel.

  QUIET.WAV   Two seconds of digital silence. The control: if this "plays" with
              a moving spectrum, the visualiser is reading uninitialised memory.

    python3 tools/mktestaudio.py [outdir]
"""
import math, os, struct, sys

RATE = 44100


def wav(path, frames, ch=2):
    """frames: list of (l, r) ints in -32768..32767."""
    data = bytearray()
    for f in frames:
        if ch == 2:
            data += struct.pack("<hh", f[0], f[1])
        else:
            data += struct.pack("<h", f[0])
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
    hdr += struct.pack("<IHHIIHH", 16, 1, ch, RATE, RATE * ch * 2, ch * 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    open(path, "wb").write(hdr + bytes(data))
    print("%s  %d frames" % (path, len(frames)))


def sine(freq, n, amp=0.35, phase=0.0):
    return [amp * math.sin(2 * math.pi * freq * i / RATE + phase) for i in range(n)]


def clip(v):
    return max(-32768, min(32767, int(v * 32767)))


def make_tone(out):
    n = RATE * 5
    s = sine(440.0, n)
    wav(os.path.join(out, "TONE.WAV"), [(clip(v), clip(v)) for v in s])


def make_sweep(out):
    """Exponential sweep: linear in frequency spends almost all its time in the
    top octave, where a 16-band analyser has one bar."""
    n = RATE * 8
    f0, f1 = 20.0, 16000.0
    k = math.log(f1 / f0) / n
    ph, fr = 0.0, []
    for i in range(n):
        f = f0 * math.exp(k * i)
        ph += 2 * math.pi * f / RATE
        v = clip(0.35 * math.sin(ph))
        fr.append((v, v))
    wav(os.path.join(out, "SWEEP.WAV"), fr)


def make_chord(out):
    n = RATE * 6
    a = sine(110.0, n, 0.20)
    b = sine(164.81, n, 0.20)
    c = sine(220.0, n, 0.20)
    fr = [(clip(a[i] + b[i] + c[i]), clip(a[i] + b[i] + c[i])) for i in range(n)]
    wav(os.path.join(out, "CHORD.WAV"), fr)


def make_pan(out):
    n = RATE * 2
    s = sine(660.0, n)
    fr = [(clip(v), 0) for v in s] + [(0, clip(v)) for v in s]
    wav(os.path.join(out, "PAN.WAV"), fr)


def make_quiet(out):
    wav(os.path.join(out, "QUIET.WAV"), [(0, 0)] * (RATE * 2))


# ---------------------------------------------------------------------------
# MOD. A 4-channel ProTracker module built by hand: 31 sample slots (the format
# has a fixed table whether or not they are used), an order list, one pattern
# of 64 rows, then the sample data.
#
# Periods are Amiga clock divisors, not hertz - the format has no concept of
# frequency, only of what Paula does with a number.
# ---------------------------------------------------------------------------
PERIOD = {"C-2": 428, "D-2": 381, "E-2": 339, "F-2": 320, "G-2": 285,
          "A-2": 254, "B-2": 226, "C-3": 214, "E-3": 169, "G-3": 143}


def make_mod(out):
    name = b"UNOAMP TEST".ljust(20, b"\0")
    body = bytearray(name)

    # Two samples: a saw (pitched, looping) and a noise burst (percussive).
    saw = bytes(((i * 8) % 256) - 128 & 0xFF for i in range(128))
    saw = bytes((v if v < 128 else v - 256) & 0xFF for v in saw)
    lfsr = 0xACE1
    noise = bytearray()
    for _ in range(512):
        lfsr = ((lfsr >> 1) ^ (0xB400 if lfsr & 1 else 0)) & 0xFFFF
        noise.append((lfsr & 0xFF))
    samples = [("SAW", saw, 0, len(saw) // 2), ("NOISE", bytes(noise), 0, 1)]

    for i in range(31):
        if i < len(samples):
            nm, data, ls, ll = samples[i]
            body += nm.encode().ljust(22, b"\0")
            body += struct.pack(">H", len(data) // 2)     # length in WORDS
            body += bytes((0, 64))                        # finetune, volume
            body += struct.pack(">HH", ls, ll)            # loop, in words
        else:
            body += b"\0" * 22 + struct.pack(">H", 0) + bytes((0, 0)) + struct.pack(">HH", 0, 1)

    body += bytes((1, 127))                               # song length, restart
    body += bytes(128)                                    # order list: pattern 0
    body += b"M.K."

    # One pattern: a bass line on channel 0, a counter-melody on 3 (both panned
    # left on Amiga), and noise hits on 1 and 2 (right).
    bass = ["C-2", None, None, None, "C-2", None, "G-2", None]
    lead = [None, None, "C-3", None, None, "E-3", None, "G-3"]
    pat = bytearray()
    for row in range(64):
        for ch in range(4):
            note, smp, eff, par = 0, 0, 0, 0
            if ch == 0 and row % 8 == 0:
                n = bass[(row // 8) % len(bass)]
                if n: note, smp = PERIOD[n], 1
            if ch == 3 and row % 8 == 2:
                n = lead[(row // 2) % len(lead)]
                if n: note, smp = PERIOD[n], 1
            if ch in (1, 2) and row % 16 == (0 if ch == 1 else 8):
                note, smp = 214, 2
            if row == 0 and ch == 0:
                eff, par = 0xF, 6                        # speed 6
            pat += bytes(((smp & 0xF0) | ((note >> 8) & 0x0F),
                          note & 0xFF,
                          ((smp & 0x0F) << 4) | eff,
                          par))
    body += pat
    for _, data, _, _ in samples:
        body += data

    p = os.path.join(out, "BEAT.MOD")
    open(p, "wb").write(bytes(body))
    print("%s  %d bytes, 4ch, 1 pattern" % (p, len(body)))


# ---------------------------------------------------------------------------
# VGM. A log of writes to an SN76489 plus waits. Version 1.50 with a real data
# offset, and a non-zero SN76489 clock so UnoAmp does not refuse it.
# ---------------------------------------------------------------------------
def psg_tone(ch, period):
    """Latch (channel, type=0) with the low 4 bits, then a data byte with the
    high 6. That two-byte shape is the chip's, not the format's."""
    return bytes((0x50, 0x80 | (ch << 5) | (period & 0x0F),
                  0x50, (period >> 4) & 0x3F))


def psg_vol(ch, atten):
    return bytes((0x50, 0x90 | (ch << 5) | (atten & 0x0F)))


def make_vgm(out):
    data = bytearray()
    for c in range(4):
        data += psg_vol(c, 15)                            # all channels off

    # A little arpeggio, then a noise hit, repeated. Periods are the chip's
    # divisor: f = 3579545 / (32 * period).
    tune = [254, 214, 190, 160, 143, 127, 143, 160]
    for rep in range(4):
        for i, per in enumerate(tune):
            data += psg_tone(0, per)
            data += psg_vol(0, 2)
            data += psg_tone(1, per * 2)
            data += psg_vol(1, 6)
            if i % 4 == 0:
                data += bytes((0x50, 0xE5))               # noise: white, mid
                data += psg_vol(3, 4)
            data += bytes((0x61,)) + struct.pack("<H", 6000)   # ~136 ms
            data += psg_vol(3, 15)
    for c in range(4):
        data += psg_vol(c, 15)
    data += bytes((0x66,))                                # end of stream

    total = 4 * len(tune) * 6000
    hdr = bytearray(0x40)
    hdr[0x00:0x04] = b"Vgm "
    struct.pack_into("<I", hdr, 0x04, 0x40 + len(data) - 4)    # EOF offset
    struct.pack_into("<I", hdr, 0x08, 0x150)                   # version 1.50
    struct.pack_into("<I", hdr, 0x0C, 3579545)                 # SN76489 clock
    struct.pack_into("<I", hdr, 0x18, total)                   # total samples
    struct.pack_into("<I", hdr, 0x28, 60)                      # rate hint
    struct.pack_into("<H", hdr, 0x2C, 0x0009)                  # SN feedback
    hdr[0x2E] = 16                                             # SR width
    struct.pack_into("<I", hdr, 0x34, 0x40 - 0x34)             # data offset
    p = os.path.join(out, "PSG.VGM")
    open(p, "wb").write(bytes(hdr) + bytes(data))
    print("%s  %d bytes, SN76489, %.1fs" % (p, 0x40 + len(data), total / 44100.0))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/testaudio"
    os.makedirs(out, exist_ok=True)
    make_tone(out); make_sweep(out); make_chord(out)
    make_pan(out); make_quiet(out); make_mod(out); make_vgm(out)


if __name__ == "__main__":
    main()
