#!/usr/bin/env python3
"""UnoDOS/pc64 Music-app verification - QEMU + OVMF, fully headless.

Where tools/audio_test.py proves the DAC path survives ExitBootServices (the
chime + the square voice), this proves the whole MEDIA chain end to end:

    file system -> streaming reads -> decoder -> resampler -> PCM FIFO -> DMA

It boots the shell with a wav-capture audiodev, opens Music, switches the
source to the ESP volume, plays a file, and asserts the captured audio really
contains the tone that file encodes - not merely that something was loud.

Driven entirely by the keyboard: the machine detaches under QEMU (the vvfat
disk sits behind AHCI and satisfies the gate), which kills the firmware
AbsolutePointer and leaves only a relative PS/2 mouse, so absolute clicking is
not reliable here. Music is fully keyboard-operable by design.

  python3 tools/music_test.py wav      TONE.WAV  -> expect ~440 Hz
  python3 tools/music_test.py midi     SONG.MID  -> expect a C-major run
  python3 tools/music_test.py both
"""
import math, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, QMP_SOCK, OVMF_CODE, OVMF_VARS

MUSIC_MENU_INDEX = 6      # 7 native apps; Music is the last (APP_MUSIC)
# what to play, and how to judge it. A single dominant frequency is the right
# test for a steady tone but a useless one for a melody - "something was loud
# at 440 Hz" would pass on a stuck note - so the MIDI file is judged on the
# SPREAD of pitches instead, which is what actually proves the sequencer,
# the tempo map and the synth are all doing their jobs.
MEDIA = {
    "wav":  ("TONE.WAV", "tone"),
    "midi": ("SONG.MID", "melody"),
    "mp3":  ("FURELISE.MP3", "melody"),
}


def vlq(v):
    out = bytearray([v & 0x7F])
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def stage_media():
    """Write the test media into the ESP tree, and return the paths so the
    caller can remove them afterwards - they are fixtures, not product, and
    must not end up in a flashed image."""
    made = []
    # A 440/660 Hz stereo tone at 44.1 kHz (also exercises the resampler, since
    # the DAC ring runs at 48 kHz). It has to outlast the capture window: the
    # player auto-advances at end of track, so a short tone finishes mid-test
    # and the next file's pitches pollute the measurement.
    rate, secs = 44100, 12
    body = bytearray()
    for i in range(rate * secs):
        for c in range(2):
            v = math.sin(2 * math.pi * (440.0 * (1.5 if c else 1.0)) * i / rate)
            body += struct.pack('<h', int(v * 0.6 * 32767))
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE' + b'fmt ' +
           struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) +
           b'data' + struct.pack('<I', len(body)))
    p = "build/esp/TONE.WAV"
    open(p, 'wb').write(hdr + bytes(body)); made.append(p)

    # a type-1 SMF: tempo/name track + a C major scale on channel 0
    div = 480
    def track(evs):
        d = bytearray()
        for delta, ev in evs:
            d += vlq(delta) + ev
        d += vlq(0) + bytes([0xFF, 0x2F, 0x00])
        return b'MTrk' + struct.pack('>I', len(d)) + bytes(d)
    title = b'UnoDOS Test Tune'
    t0 = track([(0, bytes([0xFF, 0x03, len(title)]) + title),
                (0, bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20]))])
    # repeated so the scale outlasts the capture window, for the same reason
    # the tone above is long: otherwise it ends and the next file is measured
    mel = [(0, bytes([0xC0, 0x00]))]
    for _rep in range(4):
        for nt in [60, 62, 64, 65, 67, 69, 71, 72]:
            mel.append((0, bytes([0x90, nt, 100])))
            mel.append((div, bytes([0x80, nt, 0])))
    p = "build/esp/SONG.MID"
    open(p, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 1, 2, div) + t0 + track(mel))
    made.append(p)

    # the committed demo MP3 (Fur Elise - see media/README.TXT), copied to the
    # root so every fixture sits in one listing
    if os.path.exists("media/FURELISE.MP3"):
        p = "build/esp/FURELISE.MP3"
        open(p, 'wb').write(open("media/FURELISE.MP3", 'rb').read())
        made.append(p)
    return made


def boot_and_play(which, seconds=6.0):
    target = MEDIA[which][0]
    wav = "build/music_%s.wav" % which
    if os.path.exists(wav):
        os.remove(wav)
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-audiodev", "wav,id=snd0,path=" + wav,
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
    ]
    qemu = subprocess.Popen(argv)
    q = None
    try:
        q = Qmp(QMP_SOCK)
        print("[%s] qemu up; waiting for boot..." % which)
        time.sleep(18)
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * MUSIC_MENU_INDEX))
        keys(q, "ret")
        time.sleep(2.0)
        # V cycles the source. The slots are: 0 built-in tunes, 1 the RAM disk,
        # 2 the first native FAT volume - which is the vvfat ESP holding the
        # test media. Two presses land on it.
        keys(q, "v")
        time.sleep(0.8)
        keys(q, "v")
        time.sleep(1.2)
        shot(q, "music_%s_list" % which)
        # walk the list to the target file, then Enter to play
        idx = pick_index(q, target)
        for _ in range(idx):
            keys(q, "down")
            time.sleep(0.15)
        keys(q, "ret")
        time.sleep(1.0)
        shot(q, "music_%s_playing" % which)
        time.sleep(seconds)
        shot(q, "music_%s_end" % which)
    finally:
        try:
            q.cmd("quit")               # clean exit finalises the wav header
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)
    return wav


def pick_index(q, target):
    """Row of `target` in the ESP root listing.

    The listing is plain alphabetical with directories mixed in among the
    files (they are shown so you can navigate into them), and it is FILTERED
    to media the player can open - so the fonts and HTML on the ESP are not
    rows. Computed rather than hardcoded: the numbers shifted twice already as
    the ESP gained content, and indexing the unfiltered directory silently
    played the wrong file."""
    exts = (".WAV", ".MID", ".MIDI", ".RMI", ".MP3", ".AAC", ".M4A",
            ".MP4", ".ADT")
    entries = []
    for e in os.listdir("build/esp"):
        if os.path.isdir(os.path.join("build/esp", e)) or e.upper().endswith(exts):
            entries.append(e)
    entries.sort()
    return entries.index(target) if target in entries else 0


def shot(q, tag):
    ppm = os.path.abspath("shots/%s.ppm" % tag)
    q.cmd("screendump", filename=ppm)
    time.sleep(0.7)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=False)
    try:
        os.remove(ppm)
    except OSError:
        pass


def windows(wav):
    """Per-100 ms window: (rms, dominant Hz by zero crossings). Returns the
    list plus the capture length in seconds."""
    with open(wav, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return [], 0
    pos, rate, chans, samples = 12, 44100, 2, b""
    while pos + 8 <= len(data):
        cid, sz = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", body[2:8])
        elif cid == b"data":
            samples = body
        pos += 8 + sz + (sz & 1)
    if not samples:
        return [], 0
    n = len(samples) // (2 * chans)
    mono = [struct.unpack_from("<h", samples, i * 2 * chans)[0] for i in range(n)]
    win = rate // 10
    out = []
    for w in range(len(mono) // win):
        seg = mono[w * win:(w + 1) * win]
        rms = (sum(s * s for s in seg) / win) ** 0.5
        zc = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0))
        out.append((rms, zc * 10 / 2))
    return out, len(mono) / rate


def run(which):
    target, kind = MEDIA[which]
    wav = boot_and_play(which)
    wins, secs = windows(wav)
    loud = [(r, f) for (r, f) in wins if r > 500]
    # The startup chime plays before anything is selected, so judge only the
    # tail - everything after the last quiet gap - which is the file playing.
    tail = loud[-40:] if len(loud) > 40 else loud

    if kind == "tone":
        freqs = [f for (_, f) in tail]
        med = sorted(freqs)[len(freqs) // 2] if freqs else 0
        ok = len(tail) >= 20 and med > 0 and abs(med - 440.0) / 440.0 < 0.10
        print("[%s] %s: %.1fs captured, %d loud windows, median %d Hz "
              "(expect 440)" % (which, "PASS" if ok else "FAIL", secs, len(loud), med))
    else:
        # a scale: several DISTINCT pitches spanning close to an octave
        freqs = sorted(set(int(f / 20) * 20 for (_, f) in tail if f > 80))
        span = (freqs[-1] / freqs[0]) if len(freqs) >= 2 and freqs[0] else 0
        ok = len(tail) >= 20 and len(freqs) >= 5 and span > 1.5
        print("[%s] %s: %.1fs captured, %d loud windows, %d distinct pitches "
              "%d..%d Hz (span %.2fx; expect >=5 pitches, >1.5x)"
              % (which, "PASS" if ok else "FAIL", secs, len(loud), len(freqs),
                 freqs[0] if freqs else 0, freqs[-1] if freqs else 0, span))
    return ok


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"
    modes = ["wav", "midi", "mp3"] if mode == "both" else [mode]
    fixtures = stage_media()
    try:
        ok = all([run(m) for m in modes])
    finally:
        for p in fixtures:                 # never ship the fixtures
            try:
                os.remove(p)
            except OSError:
                pass
    if not ok:
        sys.exit(1)
