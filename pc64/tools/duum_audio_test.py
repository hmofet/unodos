#!/usr/bin/env python3
"""duum_audio_test - prove pc64 really plays sampled effects and a score.

    UNO_DEBUG=1 ./build.sh && python3 tools/duum_audio_test.py

This is the gate for the four optional calls Duum asks a host for
(DUUM-REQUESTS.md, 2026-08-19): uno.sfx_load / uno.sfx_play / uno.mus_play /
uno.mus_stop.  It boots the DEBUG image in QEMU with a wav-capture audiodev,
drives the guest over URC, and then asserts about the samples the emulated DAC
actually consumed - not about calls made, and not about return values, because
a stub that returns True sounds exactly like silence.

HOW THE STAGES ARE TOLD APART.  Timing a captured stream against host
wall-clock is fragile, so nothing here depends on the order things happen in:
each stage is TAGGED BY FREQUENCY and looked for anywhere in the recording.

    1234 Hz   an effect played centred
    1700 Hz   an effect panned hard LEFT     (loud in L, quiet in R)
    2300 Hz   an effect panned hard RIGHT    (the mirror of it)
     440 Hz   the score: a Standard MIDI File built here, note 69 retriggered

and the one that matters most, because it is the thing pc64 did not have
before this landed:

    440 Hz AND 1234 Hz IN THE SAME 100 ms WINDOW - the score and an effect
    sounding together.  One-source-at-a-time shows one or the other.

The square voice cannot fake any of it: it only sounds when uno_seq_beep is
called, and this test never calls it.
"""
import os, struct, subprocess, sys, time, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import remote_qemu as RQ                                    # noqa: E402
from unoauto_remote import UnoAutoLink                      # noqa: E402

duum_mode = "duum" in sys.argv[1:]         # `duum` = the end-to-end run
WAV = os.path.join(os.path.dirname(HERE), "build",
                   "duum_audio_e2e.wav" if duum_mode else "duum_audio.wav")
SR_SFX = 11025                      # the rate a Doom DS lump carries
TONE_N = 6615                       # 0.6 s of it

fails = []


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("   " + detail) if detail else ""))
    if not cond:
        fails.append(what)


# ---- the Standard MIDI File the score stage plays ---------------------------
def smf_note(midi=69, hits=20, gap=96, division=96):
    """A type-0 SMF that retriggers one note, so its fundamental keeps coming
    back whatever envelope the synthesiser gives it.  Retriggering rather than
    holding is deliberate: a decaying patch would fade out of a spectrum test
    halfway through and read as a failure of the player.

    EVERY event carries exactly ONE delta.  Writing a trailing delta after the
    note-off *and* a leading 0 on the next note-on put two deltas in a row,
    which a correct parser reads as running status on a data byte - the file
    played its first note and then dissolved, and the mixing stage downstream
    had no score left to mix with."""
    ev = bytearray()
    ev += bytes([0x00, 0xC0, 0x10])                 # program 16, drawbar organ
    for h in range(hits):
        ev += bytes([0x00 if h == 0 else 0x08, 0x90, midi, 0x64])   # note on
        ev += bytes([gap - 8, 0x80, midi, 0x40])                    # note off
    ev += bytes([0x00, 0xFF, 0x2F, 0x00])           # end of track
    trk = b"MTrk" + struct.pack(">I", len(ev)) + bytes(ev)
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, division) + trk


# ---- capture analysis -------------------------------------------------------
def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a wav"
    pos, rate, chans, body = 12, 44100, 2, b""
    while pos + 8 <= len(data):
        cid, sz = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunk = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", chunk[2:8])
        elif cid == b"data":
            body = chunk
        pos += 8 + sz + (sz & 1)
    step = 2 * chans
    n = len(body) // step
    left = [0] * n
    right = [0] * n
    for i in range(n):
        left[i] = struct.unpack_from("<h", body, i * step)[0]
        right[i] = struct.unpack_from("<h", body, i * step + (2 if chans > 1 else 0))[0]
    return rate, left, right


def goertzel(seg, rate, hz):
    """(share, level) at one frequency.

    `share` is the magnitude divided by the window's own power - 'how much of
    what is here is THIS tone', which is what identifies a stage.  `level` is
    the plain amplitude, which is what answers 'is this tone here AT ALL'.
    The two questions need different numbers: a loud effect mixed over the
    score takes almost all of the share while the score's own level is
    unchanged, so a share test alone reads a working mixer as a failure."""
    w = 2.0 * math.cos(2.0 * math.pi * hz / rate)
    s1 = s2 = 0.0
    power = 0.0
    for s in seg:
        v = s / 32768.0
        s0 = v + w * s1 - s2
        s2, s1 = s1, s0
        power += v * v
    mag = s1 * s1 + s2 * s2 - w * s1 * s2
    if mag < 0.0:
        mag = 0.0
    level = 2.0 * math.sqrt(mag) / len(seg)
    if power <= 1e-9:
        return 0.0, level
    return mag / (power * len(seg) / 4.0), level


def windows(rate, left, right, ms=100):
    n = int(rate * ms / 1000)
    for w in range(len(left) // n):
        a, b = w * n, (w + 1) * n
        yield w * ms / 1000.0, left[a:b], right[a:b]


def rms(seg):
    return (sum(s * s for s in seg) / max(1, len(seg))) ** 0.5


# ---- the guest side ---------------------------------------------------------
TONE = ("t=lambda f: bytes([128+int(100*math.sin(6.2831853*f*i/{r}))"
        " for i in range({n})])").format(r=SR_SFX, n=TONE_N)


def boot(link):
    """RQ.boot_qemu()'s command line plus a sound device and a capture sink.

    Not a change to RQ: that file is unoautomate's harness, and every other
    gate that boots through it wants no audio device at all."""
    subprocess.run(["cp", RQ.OVMF_VARS, RQ.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-drive", "format=raw,file=" + RQ.DISK2,
        "-netdev", "user,id=n0",
        "-device", "e1000,netdev=n0",
        "-audiodev", "wav,id=snd0,path=" + WAV,
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
        "-display", "none",
    ]
    # A guest that dies mid-stage times out on the NEXT command and says
    # nothing about why, so keep the debug console reachable the same way
    # RQ does. The first failure here was a UBSan trap in the mixer.
    if os.environ.get("URC_DBGCON"):
        cmd += ["-debugcon", "file:" + os.environ["URC_DBGCON"],
                "-global", "isa-debugcon.iobase=0x402"]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def drive(link):
    def py(src, timeout=40):
        return " ".join(link.command("py", src, timeout=timeout)).strip()

    line = py("import uno; print(hasattr(uno,'sfx_load'), hasattr(uno,'sfx_play'),"
              " hasattr(uno,'mus_play'), hasattr(uno,'mus_stop'))")
    check(line.count("True") == 4, "all four calls exist on `uno`", line)

    line = py("import uno, math; " + TONE +
              "; print(uno.sfx_load(0,t(1234),%d), uno.sfx_load(1,t(1700),%d),"
              " uno.sfx_load(2,t(2300),%d))" % (SR_SFX, SR_SFX, SR_SFX), timeout=90)
    check(line.count("True") == 3, "three samples loaded", line)

    line = py("import uno; print(uno.sfx_play(0,255,128))")     # centred
    check("True" in line, "sfx_play took a voice", line)
    time.sleep(1.6)
    py("import uno; print(uno.sfx_play(1,255,0))")              # hard left
    time.sleep(1.6)
    py("import uno; print(uno.sfx_play(2,255,255))")            # hard right
    time.sleep(1.6)

    line = py("import uno; print(uno.mus_play(bytes(%s),0))" % list(smf_note()))
    check("True" in line, "mus_play accepted a Standard MIDI File", line)
    time.sleep(3.0)

    # ON TOP of it, and at a volume that does not swamp it: the question is
    # whether both are there, not which is louder.
    py("import uno; print(uno.sfx_play(0,128,128))")
    time.sleep(2.5)
    py("import uno; uno.mus_stop(); print('stopped')")
    time.sleep(1.0)


def drive_duum(link):
    """The end-to-end half: the real engine, the real WAD, on the device.

    The API stages above prove the four calls work. They cannot prove DUUM.PY
    takes them - the engine hasattr-probes at build() and turns the whole path
    off for the rest of the session if a call ever raises - so this asks the
    running app what it decided, and then checks the DAC was busy while it
    played. `app` is a module global in the same interpreter the URC `py` verb
    evaluates in, which is what makes the question askable at all."""
    def py(src, timeout=40):
        return " ".join(link.command("py", src, timeout=timeout)).strip()

    for _ in range(len(link.command("probe", timeout=15))):
        link.command("close", timeout=10)
        time.sleep(0.4)

    vol = None
    for v in range(4):
        r = py("import uno; print(uno.size(%d,'APPS/DUUM.UNO'))" % v, timeout=20)
        if r.strip().lstrip("-").isdigit() and int(r) > 0:
            vol = v
            break
    check(vol is not None, "DUUM.UNO is on a volume")
    if vol is None:
        return
    py("import uno; print(uno.run_app(%d,'APPS/DUUM.UNO'))" % vol, timeout=60)
    time.sleep(40.0)                 # WAD directory + first frame composition

    line = py("print(app.have_sfx, app.have_mus, app.err)")
    check(line.startswith("True True"), "the engine took the sampled path", line)

    for _ in range(6):               # fire: DSPISTOL, six times
        link.key(0, ord('f'), 0, timeout=8)
        time.sleep(0.7)
    time.sleep(2.0)

    line = py("print(sum(1 for s in app.sfx_state if s == 1), app.have_sfx)")
    check(line.split()[0].isdigit() and int(line.split()[0]) >= 1,
          "the engine handed pc64 at least one WAD sample", line)
    check(line.endswith("True"), "and no call raised (have_sfx still True)", line)


def analyse():
    rate, left, right = read_wav(WAV)
    print("  captured %.1f s at %d Hz" % (len(left) / float(rate), rate))
    hits = {1234: [], 1700: [], 2300: [], 440: []}
    levels = []                      # (t, level440, level1234) per loud window
    for t, l, r in windows(rate, left, right):
        if rms(l) < 300 and rms(r) < 300:
            continue
        mono = [(a + b) // 2 for a, b in zip(l, r)]
        e = dict((hz, goertzel(mono, rate, hz)) for hz in hits)
        for hz in hits:
            if e[hz][0] > 0.25:
                hits[hz].append((t, rms(l), rms(r)))
        levels.append((t, e[440][1], e[1234][1]))

    # Present-at-all floors, taken from this recording rather than guessed: a
    # quarter of the loudest window each tone ever reaches.
    top440 = max([x[1] for x in levels] or [0.0])
    top1234 = max([x[2] for x in levels] or [0.0])
    both = [t for t, a, b in levels if a > top440 / 4 and b > top1234 / 4]

    for hz in (440, 1234, 1700, 2300):
        print("  %4d Hz: %d window(s)" % (hz, len(hits[hz])))
    print("  peak level: 440 Hz %.4f, 1234 Hz %.4f" % (top440, top1234))
    check(len(hits[1234]) >= 3, "a centred effect reached the DAC (1234 Hz)")
    check(len(hits[440]) >= 8, "the score reached the DAC (440 Hz)")

    for hz, side, want_left in ((1700, "LEFT", True), (2300, "RIGHT", False)):
        what = "sep=%d is heard on the %s" % (0 if want_left else 255, side)
        if not hits[hz]:
            check(False, what, "no %d Hz windows" % hz)
            continue
        la = sum(x[1] for x in hits[hz]) / len(hits[hz])
        ra = sum(x[2] for x in hits[hz]) / len(hits[hz])
        check((la > 3 * ra) if want_left else (ra > 3 * la), what,
              "L %.0f vs R %.0f" % (la, ra))

    check(len(both) >= 2, "score and effect sound TOGETHER (mixed, not chosen)",
          "%d window(s)" % len(both))


def analyse_duum():
    rate, left, right = read_wav(WAV)
    print("  captured %.1f s at %d Hz" % (len(left) / float(rate), rate))
    loud = 0
    for t, l, r in windows(rate, left, right):
        if rms(l) > 400 or rms(r) > 400:
            loud += 1
    print("  %d loud 100 ms window(s)" % loud)
    check(loud >= 20, "Duum kept the DAC busy (score + effects)",
          "%d windows" % loud)


def main():
    if not os.path.exists(os.path.join(RQ.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1
    if os.path.exists(WAV):
        os.remove(WAV)
    os.makedirs(os.path.dirname(WAV), exist_ok=True)

    RQ.build_disk()
    link = UnoAutoLink(port=RQ.PORT)
    link.listen()
    qemu = boot(link)
    try:
        if not link.wait_connected(240.0):
            print("FAIL: the guest never dialled in - is this the DEBUG build?")
            return 1
        link.wait_hello(30.0)
        time.sleep(3.0)
        if duum_mode:
            drive_duum(link)
        else:
            drive(link)
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        try:
            link.close()
        except Exception:
            pass

    if not os.path.exists(WAV):
        print("FAIL: no capture at " + WAV)
        return 1
    if duum_mode:
        analyse_duum()
    else:
        analyse()

    print()
    if fails:
        print("DUUM AUDIO: %d FAILURE(S)" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("DUUM AUDIO: PASS (capture kept at %s)" % WAV)
    return 0


sys.exit(main())
