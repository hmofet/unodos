#!/usr/bin/env python3
"""unomedia audio decoder test runner (host) - the audio twin of run_tests.py.

Builds audtest (hosted gcc + ASan/UBSan over the same sources the kernel
compiles freestanding), generates references with ffmpeg, decodes each with
audtest, and compares PCM:

  WAV s16          -> sample-exact
  WAV 24-bit/float -> PSNR >= 80 dB (both sides truncate differently)
  MP3              -> aligned PSNR >= 55 dB vs ffmpeg's decode of the SAME
                      file (both follow the spec; codec delay differs, so the
                      compare searches a small alignment window first)
  AAC (M4A/ADTS)   -> aligned PSNR >= 50 dB vs ffmpeg's decode
  MIDI             -> a synthesiser, not a decoder: assert rate/duration
                      sanity and that the output is actually sounding

Usage: python3 run_audio_tests.py [filter-substring]
"""
import math, os, struct, subprocess, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
UM   = os.path.dirname(HERE)
GEN  = os.path.join(HERE, "gen")
BIN  = os.path.join(GEN, "audtest")

PASS = []; FAIL = []


def sh(*args):
    return subprocess.run(list(args), check=True, capture_output=True)


def build():
    os.makedirs(GEN, exist_ok=True)
    srcs = [os.path.join(UM, f) for f in sorted(os.listdir(UM))
            if f.endswith(".c")]
    srcs.append(os.path.join(HERE, "audtest.c"))
    sh("gcc", "-O2", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra",
       "-I", UM, "-o", BIN, *srcs, "-lm")


def check(name, ok, why=""):
    (PASS if ok else FAIL).append(name)
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + why))


def decode(path, seek=None):
    out = os.path.join(GEN, "aud.raw")
    cmd = [BIN, path, out] + ([str(seek)] if seek is not None else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    raw = open(out, "rb").read() if os.path.exists(out) else b""
    return r.returncode, r.stdout.strip(), raw


def ffmpeg_pcm(path, rate, ch):
    """ffmpeg's decode of `path` as s16 interleaved at rate/ch."""
    return sh("ffmpeg", "-v", "quiet", "-i", path, "-f", "s16le",
              "-acodec", "pcm_s16le", "-ar", str(rate), "-ac", str(ch),
              "-").stdout


def samples(raw):
    return struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2])


def psnr(a, b):
    n = min(len(a), len(b))
    if n == 0: return 0.0
    se = sum((a[i] - b[i]) ** 2 for i in range(n))
    if se == 0: return 99.0
    return 10.0 * math.log10(32768.0 * 32768.0 * n / se)


def aligned_psnr(ours, ref, ch, window=4096):
    """Best PSNR over a +-window frame alignment (codec delay differs)."""
    a, b = samples(ours), samples(ref)
    # coarse correlation search on a subsampled mono fold
    am = [a[i] + a[i + ch - 1] for i in range(0, min(len(a), 400000) - ch, ch)]
    bm = [b[i] + b[i + ch - 1] for i in range(0, min(len(b), 400000) - ch, ch)]
    best_off, best_c = 0, None
    for off in range(-window, window + 1, 16):
        c = 0
        for i in range(0, 20000, 8):
            j = i + off
            if 0 <= j < len(bm) and i < len(am):
                c += am[i] * bm[j]
        if best_c is None or c > best_c:
            best_c, best_off = c, off
    # refine +-16 around the coarse peak
    best_fine, best_db = best_off, -1.0
    for off in range(best_off - 16, best_off + 17):
        ai = a[max(0, off * ch) if off < 0 else 0:]
        bi = b[off * ch if off > 0 else 0:] if off > 0 else b[-off * ch:] if off < 0 else b
        # align: positive off means ref leads
        if off >= 0:
            ai, bi = a, b[off * ch:]
        else:
            ai, bi = a[-off * ch:], b
        n = min(len(ai), len(bi), 200000)
        if n <= 0: continue
        se = 0
        for i in range(n):
            d = ai[i] - bi[i]; se += d * d
        db = 99.0 if se == 0 else 10.0 * math.log10(32768.0 * 32768.0 * n / se)
        if db > best_db: best_db, best_fine = db, off
    return best_db, best_fine


def parse_head(head):
    d = {}
    parts = head.split()
    d["fmt"] = parts[0] if parts else "?"
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            d[k] = int(v)
    return d


def gen_tone_wav(path, codec="pcm_s16le", secs=3):
    sh("ffmpeg", "-v", "quiet", "-y", "-f", "lavfi",
       "-i", "sine=frequency=440:sample_rate=44100:duration=%d" % secs,
       "-af", "aeval=val(0)|0.5*val(0)",     # stereo, distinct channels
       "-acodec", codec, path)


def gen_sweep(path, codec, *extra):
    sh("ffmpeg", "-v", "quiet", "-y", "-f", "lavfi",
       "-i", "aevalsrc=0.4*sin(2*PI*t*(200+300*t)):s=44100:d=4",
       "-ac", "2", "-acodec", codec, *extra, path)


def midi_bytes():
    """A tiny type-0 SMF: four quarter notes, 120 bpm."""
    ev = b""
    for i, note in enumerate((60, 64, 67, 72)):
        ev += (b"\x00" if i == 0 else b"\x83\x60") + bytes((0x90, note, 100))
        ev += b"\x83\x60" + bytes((0x80, note, 0))
    trk = b"\x00\xff\x51\x03\x07\xa1\x20" + ev + b"\x00\xff\x2f\x00"
    return (b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480)
            + b"MTrk" + struct.pack(">I", len(trk)) + trk)


def main():
    filt = sys.argv[1] if len(sys.argv) > 1 else ""
    build()
    g = lambda n: os.path.join(GEN, n)

    # ---- WAV ----
    if filt in "wav_s16":
        p = g("wav_s16.wav"); gen_tone_wav(p)
        rc, head, raw = decode(p)
        h = parse_head(head) if rc == 0 else {}
        ref = ffmpeg_pcm(p, 44100, 2) if rc == 0 else b""
        check("wav_s16", rc == 0 and h.get("rate") == 44100
              and h.get("ch") == 2 and raw == ref,
              head if rc else "mismatch")
    for codec, name in [("pcm_s24le", "wav_s24"), ("pcm_f32le", "wav_f32")]:
        if filt not in name: continue
        p = g(name + ".wav"); gen_tone_wav(p, codec)
        rc, head, raw = decode(p)
        if rc != 0: check(name, False, head); continue
        db = psnr(samples(raw), samples(ffmpeg_pcm(p, 44100, 2)))
        check(name, db >= 80.0, "PSNR %.1f dB (%s)" % (db, head))

    # ---- MP3 ----
    if filt in "mp3_sweep":
        p = g("mp3_sweep.mp3"); gen_sweep(p, "libmp3lame", "-b:a", "192k")
        rc, head, raw = decode(p)
        if rc != 0: check("mp3_sweep", False, head)
        else:
            db, off = aligned_psnr(raw, ffmpeg_pcm(p, 44100, 2), 2)
            check("mp3_sweep", db >= 55.0,
                  "PSNR %.1f dB @off %d (%s)" % (db, off, head))

    # ---- AAC (decoder lands with the AAC milestone; these gate it) ----
    if filt in "aac_m4a":
        p = g("aac_m4a.m4a"); gen_sweep(p, "aac", "-b:a", "128k")
        rc, head, raw = decode(p)
        if rc != 0: check("aac_m4a", False, head)
        else:
            db, off = aligned_psnr(raw, ffmpeg_pcm(p, 44100, 2), 2)
            check("aac_m4a", db >= 50.0,
                  "PSNR %.1f dB @off %d (%s)" % (db, off, head))
    if filt in "aac_adts":
        p = g("aac_adts.aac"); gen_sweep(p, "aac", "-b:a", "128k", "-f", "adts")
        rc, head, raw = decode(p)
        if rc != 0: check("aac_adts", False, head)
        else:
            db, off = aligned_psnr(raw, ffmpeg_pcm(p, 44100, 2), 2)
            check("aac_adts", db >= 50.0,
                  "PSNR %.1f dB @off %d (%s)" % (db, off, head))

    # ---- MIDI (synth: sanity, not sample compare) ----
    if filt in "midi_notes":
        p = g("midi_notes.mid")
        open(p, "wb").write(midi_bytes())
        rc, head, raw = decode(p)
        h = parse_head(head) if rc == 0 else {}
        ok = rc == 0 and h.get("frames", 0) > 0
        if ok:
            s = samples(raw)
            peak = max(abs(x) for x in s) if s else 0
            secs = h["frames"] / float(h.get("rate", 1) or 1)
            ok = peak > 2000 and 3.0 <= secs <= 6.0   # 4 quarters @120bpm ~4s
        check("midi_notes", ok, head)

    print("\n%d passed, %d failed" % (len(PASS), len(FAIL)))
    if FAIL: sys.exit(1)


if __name__ == "__main__":
    main()
