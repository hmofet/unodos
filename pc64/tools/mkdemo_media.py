#!/usr/bin/env python3
"""Generate the bundled demo media for the Music app.

LICENSING - the whole point of generating rather than downloading:

Every byte here is produced by this script. The music is composed in code
(scales, arpeggios and a chord progression written below), the WAVs are
rendered by UnoDOS's OWN MIDI synthesiser (dec_midi.c compiled for the host),
and the MP3s are encoded from those WAVs. No third-party sample, recording,
performance or composition is involved at any stage, so the results are
unambiguously ours to ship: they are released into the PUBLIC DOMAIN (CC0),
with NO attribution required. media/README.TXT records that alongside them.

This is a generator, not part of the build: its outputs are committed under
pc64/media/ and build.sh only copies them. Regenerate with

  python3 tools/mkdemo_media.py

Needs gcc (to build the host renderer) and ffmpeg with libmp3lame.
"""
import os, struct, subprocess, sys, math

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
OUT = "media"
TMP = "build/mediatmp"


# ---- MIDI writing ----------------------------------------------------------
def vlq(v):
    out = bytearray([v & 0x7F])
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def meta_text(kind, s):
    """A text meta event with the length taken FROM THE STRING. Hardcoding it
    is how the first cut of this script shipped a name event declaring 15
    bytes and supplying 14: every parser then read one byte too many and the
    rest of the track decoded as garbage, which showed up as a file that
    claimed to be twice its real length."""
    b = s.encode("ascii")
    return bytes([0xFF, kind, len(b)]) + b


def track(events):
    d = bytearray()
    for delta, ev in events:
        d += vlq(delta) + ev
    d += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    return b"MTrk" + struct.pack(">I", len(d)) + bytes(d)


def smf(path, tracks, div=480, fmt=1):
    data = b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), div)
    for t in tracks:
        data += t
    open(path, "wb").write(data)
    print("  wrote %s (%d bytes)" % (path, len(data)))


DIV = 480
Q, E, H, W = DIV, DIV // 2, DIV * 2, DIV * 4
# One pass through the progression, not two. These files live in the repo, and
# a 44.1 kHz stereo WAV costs ~170 KB per second - doubling the length doubles
# what every clone carries forever to hear the same eight bars twice.
REPS = 1
BARS = 4


def compose_fur_elise():
    """Beethoven, Fur Elise (WoO 59) - the opening period.

    Chosen because it is instantly recognisable: for ear-testing a decoder you
    want a tune where WRONG is obvious, and a phrase everyone knows beats an
    original one. The composition is public domain (Beethoven died 1827) and
    this is our own transcription rendered by our own synth, so there is no
    recording or arrangement right either - it stays CC0 like everything else
    here."""
    meta = track([(0, meta_text(0x03, "Fur Elise - Beethoven")),
                  (0, bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 400000)[1:])])

    # right hand, in eighths unless marked
    E5, DS5, B4, D5, C5, A4 = 76, 75, 71, 74, 72, 69
    C4, E4, GS4, A3n, B3 = 60, 64, 68, 57, 59
    phrase = [
        (E5, E), (DS5, E), (E5, E), (DS5, E), (E5, E), (B4, E), (D5, E), (C5, E),
        (A4, Q),
        (C4, E), (E4, E), (A4, E), (B4, Q),
        (E4, E), (GS4, E), (B4, E), (C5, Q),
        (E4, E),
    ]
    mel = [(0, bytes([0xC0, 0]))]
    for rep in range(2):
        for n, d in phrase:
            mel.append((0, bytes([0x90, n, 88])))
            mel.append((d, bytes([0x80, n, 0])))
        # the answering half of the period, second time through
        if rep:
            for n, d in [(E5, E), (DS5, E), (E5, E), (DS5, E), (E5, E), (B4, E),
                         (D5, E), (C5, E), (A4, Q), (C4, E), (E4, E), (A4, E),
                         (B4, Q), (E4, E), (C5, E), (B4, E), (A4, H)]:
                mel.append((0, bytes([0x90, n, 88])))
                mel.append((d, bytes([0x80, n, 0])))

    # left hand: the broken-chord accompaniment under the held notes
    A2, E3, A3, E2, GS3 = 45, 52, 57, 40, 56
    bass = [(0, bytes([0xC1, 0]))]
    def arp(root, mid, top, gap):
        bass.append((gap, bytes([0x91, root, 62])))
        bass.append((E, bytes([0x81, root, 0])))
        bass.append((0, bytes([0x91, mid, 58])))
        bass.append((E, bytes([0x81, mid, 0])))
        bass.append((0, bytes([0x91, top, 58])))
        bass.append((E, bytes([0x81, top, 0])))
    for rep in range(2):
        arp(A2, E3, A3, E * 8)      # under the A
        arp(E2, E3, GS3, Q)         # under the B
        arp(A2, E3, A3, Q)          # under the C
        if rep:
            arp(A2, E3, A3, Q * 6)
    smf(os.path.join(OUT, "FURELISE.MID"), [meta, track(mel), track(bass)])


def compose_ode():
    """Beethoven, Ode to Joy - melody plus a simple chordal accompaniment.
    Public domain, our transcription, our synth (see compose_fur_elise)."""
    meta = track([(0, meta_text(0x03, "Ode to Joy - Beethoven")),
                  (0, bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 500000)[1:])])
    C4, D4, E4, F4, G4 = 60, 62, 64, 65, 67
    line = [(E4, Q), (E4, Q), (F4, Q), (G4, Q), (G4, Q), (F4, Q), (E4, Q), (D4, Q),
            (C4, Q), (C4, Q), (D4, Q), (E4, Q), (E4, Q + E), (D4, E), (D4, H),
            (E4, Q), (E4, Q), (F4, Q), (G4, Q), (G4, Q), (F4, Q), (E4, Q), (D4, Q),
            (C4, Q), (C4, Q), (D4, Q), (E4, Q), (D4, Q + E), (C4, E), (C4, H)]
    mel = [(0, bytes([0xC0, 0]))]
    for n, d in line:
        mel.append((0, bytes([0x90, n, 92])))
        mel.append((d, bytes([0x80, n, 0])))

    # one chord per bar: C  G  Am  F ... under the phrase
    chords = [(48, 52, 55), (43, 47, 50), (45, 48, 52), (41, 45, 48)]
    ch = [(0, bytes([0xC1, 48]))]              # strings underneath
    for bar in range(8):
        c = chords[bar % 4]
        for n in c:
            ch.append((0, bytes([0x91, n, 55])))
        ch.append((W, bytes([0x81, c[0], 0])))
        for n in c[1:]:
            ch.append((0, bytes([0x81, n, 0])))
    smf(os.path.join(OUT, "ODEJOY.MID"), [meta, track(mel), track(ch)])


def compose_demo():
    """An original 8-bar piece: a I-V-vi-IV progression in C with a melody
    over it and a simple kick/hat pattern. Written here as note numbers -
    there is no source work being transcribed."""
    meta = track([
        (0, meta_text(0x03, "UnoDOS Demo")),
        (0, bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 500000)[1:]),
    ])

    # chords: C  G  Am F, two bars each half-note
    chords = [(60, 64, 67), (55, 59, 62), (57, 60, 64), (53, 57, 60)]
    ch = [(0, bytes([0xC1, 48]))]                 # program 48 = strings
    for rep in range(REPS):
        for c in chords:
            for n in c:
                ch.append((0, bytes([0x91, n, 70])))
            ch.append((H, bytes([0x81, c[0], 0])))
            for n in c[1:]:
                ch.append((0, bytes([0x81, n, 0])))
    chord_trk = track(ch)

    # melody: an ascending/descending line over the progression
    mel = [(0, bytes([0xC0, 0]))]                 # program 0 = piano
    line = [72, 76, 79, 76, 74, 71, 67, 71,
            72, 69, 65, 69, 72, 76, 72, 67]
    for rep in range(REPS):
        for i, n in enumerate(line):
            mel.append((0, bytes([0x90, n, 90 if i % 4 == 0 else 72])))
            mel.append((E, bytes([0x80, n, 0])))
    mel_trk = track(mel)

    # drums on channel 10 (index 9): kick on 1 and 3, hat on eighths
    dr = []
    for bar in range(BARS):
        for eighth in range(8):
            hit = 36 if eighth in (0, 4) else None
            if hit:
                dr.append((0, bytes([0x99, hit, 100])))
            dr.append((0, bytes([0x99, 42, 60])))
            dr.append((E, bytes([0x89, 42, 0])))
            if hit:
                dr.append((0, bytes([0x89, hit, 0])))
    drum_trk = track(dr)

    smf(os.path.join(OUT, "DEMO.MID"), [meta, chord_trk, mel_trk, drum_trk])


def compose_scale():
    """A bare C-major scale - the file to reach for when something sounds
    wrong and you want to hear exactly one note at a time."""
    meta = track([(0, meta_text(0x03, "Scale Test")),
                  (0, bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 500000)[1:])])
    mel = [(0, bytes([0xC0, 0]))]
    for n in [60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60]:
        mel.append((0, bytes([0x90, n, 100])))
        mel.append((Q, bytes([0x80, n, 0])))
    smf(os.path.join(OUT, "SCALE.MID"), [meta, track(mel)])


# ---- WAV helpers ------------------------------------------------------------
def write_wav(path, rate, ch, bits, frames_bytes):
    align = ch * bits // 8
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(frames_bytes)) + b"WAVE" +
           b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, rate,
                                 rate * align, align, bits) +
           b"data" + struct.pack("<I", len(frames_bytes)))
    open(path, "wb").write(hdr + frames_bytes)
    print("  wrote %s (%d KB)" % (path, (len(frames_bytes) + 44) // 1024))


def make_tones():
    """A clean stereo reference signal: four held tones (a C major arpeggio),
    44.1 kHz 16-bit - what the automated QEMU test measures against.

    Two things here are deliberate, because the first version of this file
    sounded like clicks and static on real hardware:

      * the phase ACCUMULATES rather than being recomputed as sin(2*pi*f*i/r).
        Recomputing means that when f changes the waveform jumps to a new point
        in its cycle - a full-scale discontinuity, i.e. a loud click at every
        note boundary.
      * the envelope is per-NOTE. The old one used `i % rate`, which silently
        re-triggered the attack once a second whether or not a note started
        there, chopping the tone into pieces.

    A tone that clicks is indistinguishable from a decoder that is broken, so
    the reference signal has to be clean before it can measure anything."""
    rate, secs_per = 44100, 1.2
    notes = [261.63, 329.63, 392.00, 523.25]      # C4 E4 G4 C5
    body = bytearray()
    phase = 0.0
    for f in notes:
        n = int(rate * secs_per)
        ramp = int(rate * 0.01)                    # 10 ms in/out, click-free
        for i in range(n):
            env = 1.0
            if i < ramp:            env = i / ramp
            elif i > n - ramp:      env = (n - i) / ramp
            v = math.sin(phase) * 0.55 * env
            phase += 2.0 * math.pi * f / rate
            if phase > 2.0 * math.pi:
                phase -= 2.0 * math.pi
            s = int(v * 32767)
            body += struct.pack("<hh", s, s)
    write_wav(os.path.join(OUT, "TONES.WAV"), rate, 2, 16, bytes(body))


# ---- render MIDI through our own synth --------------------------------------
def build_renderer():
    os.makedirs(TMP, exist_ok=True)
    exe = os.path.join(TMP, "render")
    src = os.path.join(TMP, "render.c")
    open(src, "w").write(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unomedia.h"
static FILE *g_f;
static long src_read(void *x, long off, unsigned char *b, long m)
{ (void)x; if (fseek(g_f,off,SEEK_SET)) return -1;
  return (long)fread(b,1,(size_t)m,g_f); }
static void *xa(unsigned long n){ return malloc(n); }
static void p32(FILE*f,unsigned v){fputc(v&255,f);fputc((v>>8)&255,f);fputc((v>>16)&255,f);fputc((v>>24)&255,f);}
static void p16(FILE*f,unsigned v){fputc(v&255,f);fputc((v>>8)&255,f);}
int main(int c, char**v){
  um_src src; um_audio_info info; short buf[4096*2]; FILE*o; long tot=0; int n;
  if(c<3) return 2;
  um_set_alloc(xa, free);
  g_f=fopen(v[1],"rb"); if(!g_f){perror(v[1]);return 2;}
  fseek(g_f,0,SEEK_END); src.size=ftell(g_f); rewind(g_f);
  src.read=src_read; src.ctx=0;
  if(!um_audio_open(&src,v[1],&info)){fprintf(stderr,"no decoder: %s\n",um_error());return 1;}
  o=fopen(v[2],"wb"); if(!o) return 2;
  fwrite("RIFF",1,4,o); p32(o,0); fwrite("WAVE",1,4,o);
  fwrite("fmt ",1,4,o); p32(o,16); p16(o,1); p16(o,info.channels);
  p32(o,info.rate); p32(o,info.rate*info.channels*2);
  p16(o,info.channels*2); p16(o,16);
  fwrite("data",1,4,o); p32(o,0);
  while((n=um_audio_decode(buf,4096))>0){
    fwrite(buf,sizeof(short)*info.channels,(size_t)n,o); tot+=n;
    if(tot>(long)info.rate*300) break;
  }
  { long b=tot*info.channels*2;
    fseek(o,4,SEEK_SET); p32(o,(unsigned)(36+b));
    fseek(o,40,SEEK_SET); p32(o,(unsigned)b); }
  fclose(o);
  printf("  rendered %s -> %s (%.1fs, %d Hz)\n", v[1], v[2],
         (double)tot/info.rate, info.rate);
  return tot>0?0:1;
}
''')
    um = os.path.join("..", "unomedia")
    subprocess.run(["gcc", "-O2", "-I" + um, "-o", exe, src]
                   + [os.path.join(um, f + ".c") for f in
                      ["unomedia", "um_audio", "um_wav", "um_midi",
                       "um_mp3", "um_aac"]]
                   + ["-lm"], check=True)
    return exe


def render(exe, mid, wav):
    """Render, then peak-normalise to -3 dBFS. The live synth deliberately
    keeps headroom for chords to sum into, which leaves a single rendered file
    quieter than a listener expects from a music file; these are masters, so
    they get levelled once here rather than by turning the synth up until it
    clips in the general case."""
    raw = wav + ".raw.wav"
    subprocess.run([exe, mid, raw], check=True)
    # measure the true peak, then apply ONE fixed gain. A plain peak normalise,
    # not a dynamics processor: the arrangement's own loud/quiet shape is the
    # synth's output and should survive intact.
    probe = subprocess.run(["ffmpeg", "-hide_banner", "-i", raw,
                            "-af", "volumedetect", "-f", "null", os.devnull],
                           capture_output=True, text=True)
    peak = 0.0
    for ln in probe.stderr.splitlines():
        if "max_volume:" in ln:
            peak = float(ln.split("max_volume:")[1].split("dB")[0])
    gain = -3.0 - peak                       # land the peak at -3 dBFS
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw,
                    "-af", "volume=%.2fdB" % gain,
                    "-c:a", "pcm_s16le", wav], check=True)
    os.remove(raw)
    print("    normalised %+.1f dB (peak was %.1f dBFS)" % (gain, peak))


def downmix(src, dst, rate):
    """Mono at a lower rate - keeps a recognisable WAV in the repo at a
    quarter the bytes, and exercises the 48 kHz resampler from a rate that
    isn't a neat ratio."""
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src,
                    "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", dst],
                   check=True)
    print("  wrote %s (%d KB, %d Hz mono)"
          % (dst, os.path.getsize(dst) // 1024, rate))


def to_mp3(wav, mp3, kbps="128"):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", wav,
                    "-codec:a", "libmp3lame", "-b:a", kbps + "k", mp3], check=True)
    print("  wrote %s (%d KB, %s kbps)" % (mp3, os.path.getsize(mp3) // 1024, kbps))


def to_m4a(wav, m4a, kbps="128"):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", wav,
                    "-codec:a", "aac", "-b:a", kbps + "k", m4a], check=True)
    print("  wrote %s (%d KB, %s kbps AAC)" % (m4a, os.path.getsize(m4a) // 1024, kbps))


README = """UnoDOS demo media
=================

These files are here to exercise the Music app's decoders.

LICENCE: PUBLIC DOMAIN (CC0 1.0). No rights reserved, NO ATTRIBUTION REQUIRED.
You may copy, modify, redistribute and sell them, for any purpose.

They can be released that way because nothing in them came from anywhere else.
Every file was generated by pc64/tools/mkdemo_media.py:

  * the music is composed in code - a chord progression, a melody line and a
    drum pattern written directly as note numbers in that script. No existing
    composition is transcribed or arranged.
  * the WAVs are rendered by UnoDOS's own MIDI synthesiser (unomedia's
    um_midi.c built for the host), so even the instrument sounds are ours -
    there is no sampled instrument, soundfont or recording involved.
  * the MP3s are encoded from those WAVs with LAME, the M4A with ffmpeg's
    AAC encoder.

The two tunes are RECOGNISABLE on purpose: when you are ear-testing a decoder,
a phrase you already know makes "wrong" obvious in a way an original piece
never does. Both are Beethoven - public domain as compositions - transcribed
here and performed by our own synth, so no recording or arrangement right
attaches either.

Files
-----
  FURELISE.MID  Fur Elise (Beethoven), melody + broken-chord left hand
  FURELISE.MP3  the same, rendered and encoded at 128 kbps
  ODEJOY.MID    Ode to Joy (Beethoven), melody + string chords
  ODEJOY.WAV    the same, 22.05 kHz mono PCM - also exercises the resampler,
                since the DAC ring runs at 48 kHz
  ODEJOY.M4A    the same, 128 kbps AAC-LC - the unomedia AAC decoder's
                out-of-box exercise
  SCALE.MID     a bare C-major scale up and down - one note at a time, for
                when you want to hear exactly what the synth is doing
  SCALE.MP3     the scale at 128 kbps, simple material for A/B checks
  TONES.WAV     four held tones (C E G C), 44.1 kHz stereo - the reference
                signal the automated QEMU test measures. Phase-continuous with
                per-note ramps, so any click you hear is the OS, not the file

Regenerate with:  python3 tools/mkdemo_media.py   (needs gcc + ffmpeg/LAME)
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    print("composing...")
    compose_fur_elise()
    compose_ode()
    compose_scale()
    make_tones()
    print("building the host renderer (UnoDOS's own MIDI synth)...")
    exe = build_renderer()
    print("rendering + encoding...")
    # Fur Elise ships as MIDI + MP3, Ode to Joy as MIDI + WAV: between them
    # every decoder gets a tune you can hum, without carrying two big PCM
    # files in the repo. A 44.1 kHz stereo WAV is ~170 KB per second.
    fe_wav = os.path.join(TMP, "furelise.wav")
    render(exe, os.path.join(OUT, "FURELISE.MID"), fe_wav)
    to_mp3(fe_wav, os.path.join(OUT, "FURELISE.MP3"))
    ode_wav = os.path.join(TMP, "odejoy.wav")
    render(exe, os.path.join(OUT, "ODEJOY.MID"), ode_wav)
    downmix(ode_wav, os.path.join(OUT, "ODEJOY.WAV"), 22050)
    to_m4a(ode_wav, os.path.join(OUT, "ODEJOY.M4A"))
    scale_wav = os.path.join(TMP, "scale.wav")
    render(exe, os.path.join(OUT, "SCALE.MID"), scale_wav)
    to_mp3(scale_wav, os.path.join(OUT, "SCALE.MP3"))
    open(os.path.join(OUT, "README.TXT"), "w", newline="\r\n").write(README)
    print("  wrote %s/README.TXT" % OUT)
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("done: %d files, %d KB total" % (len(os.listdir(OUT)), total // 1024))


if __name__ == "__main__":
    main()
